#include "b+tree-engine.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <unistd.h>
#include <sys/stat.h>

namespace {

constexpr std::size_t PAGE_SIZE = 4096;
constexpr double SPLIT_OCCUPANCY = 0.88;
constexpr double MERGE_OCCUPANCY = 0.28;
constexpr char PAGE_MAGIC[] = "BTPG";
constexpr char MANIFEST_MAGIC[8] = {'B', 'T', 'M', 'A', 'N', '0', '1', '\0'};
constexpr char WAL_MAGIC[8] = {'B', 'T', 'W', 'A', 'L', '0', '1', '\0'};
constexpr std::uint32_t FORMAT_VERSION = 1;

#pragma pack(push, 1)
struct PageHeader {
    char magic[4];
    std::uint32_t page_id;
    std::uint32_t parent_id;
    std::uint32_t next_page_id;
    std::uint32_t prev_page_id;
    std::uint16_t slot_count;
    std::uint16_t free_start;
    std::uint8_t is_leaf;
    std::uint8_t reserved[3];
};

struct Slot {
    std::uint16_t offset;
    std::uint16_t size;
};

struct ManifestDisk {
    char magic[8];
    std::uint32_t page_size;
    std::uint32_t root_page_id;
    std::uint32_t highest_page_id;
};

struct WalHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t old_root_page_id;
    std::uint32_t old_highest_page_id;
};

struct WalRecordHeader {
    std::uint8_t type;
    std::uint32_t page_id;
    std::uint32_t size;
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == 28, "unexpected page header layout");
static_assert(sizeof(Slot) == 4, "unexpected slot layout");

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
}

void close_nointr(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

void write_all(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size != 0) {
        const ssize_t count = ::write(fd, bytes, size);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("write");
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
}

bool read_all_or_eof(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    while (size != 0) {
        const ssize_t count = ::read(fd, bytes, size);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("read");
        }
        if (count == 0) {
            return false;
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

void pwrite_all(int fd, const void* data, std::size_t size, off_t offset) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size != 0) {
        const ssize_t count = ::pwrite(fd, bytes, size, offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pwrite");
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
        offset += count;
    }
}

void pread_all(int fd, void* data, std::size_t size, off_t offset) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    while (size != 0) {
        const ssize_t count = ::pread(fd, bytes, size, offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("pread");
        }
        if (count == 0) {
            throw std::runtime_error("B+ tree page file is truncated");
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
        offset += count;
    }
}

off_t page_offset(std::uint32_t page_id) {
    if (page_id == 0) {
        throw std::runtime_error("page id zero is invalid");
    }
    return static_cast<off_t>(page_id - 1) * static_cast<off_t>(PAGE_SIZE);
}

void ensure_directory(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) < 0 && errno != EEXIST) {
        fail("mkdir");
    }
    struct stat info {};
    if (::stat(path.c_str(), &info) < 0) {
        fail("stat");
    }
    if (!S_ISDIR(info.st_mode)) {
        throw std::runtime_error("B+ tree path is not a directory: " + path);
    }
}

void sync_fd(int fd) {
    if (::fdatasync(fd) < 0) {
        fail("fdatasync");
    }
}

void sync_directory(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fail("open directory");
    }
    if (::fsync(fd) < 0) {
        const int saved = errno;
        close_nointr(fd);
        errno = saved;
        fail("fsync directory");
    }
    close_nointr(fd);
}

std::uint16_t read_u16(const std::uint8_t* data) {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::uint32_t read_u32(const std::uint8_t* data) {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

void write_u16(std::uint8_t* data, std::uint16_t value) {
    std::memcpy(data, &value, sizeof(value));
}

void write_u32(std::uint8_t* data, std::uint32_t value) {
    std::memcpy(data, &value, sizeof(value));
}

std::size_t leaf_record_size(const KVPair& entry) {
    return 4 + entry.first.size() + entry.second.size();
}

std::size_t internal_record_size(const std::string& key) {
    return 2 + key.size() + 4;
}

std::size_t leaf_used(const std::vector<KVPair>& entries) {
    std::size_t size = sizeof(PageHeader) + entries.size() * sizeof(Slot);
    for (const auto& entry : entries) {
        size += leaf_record_size(entry);
    }
    return size;
}

std::size_t internal_used(const std::vector<std::string>& keys) {
    std::size_t size = sizeof(PageHeader) + keys.size() * sizeof(Slot);
    for (const std::string& key : keys) {
        size += internal_record_size(key);
    }
    return size;
}

double occupancy(std::size_t used) {
    return static_cast<double>(used - sizeof(PageHeader)) /
           static_cast<double>(PAGE_SIZE - sizeof(PageHeader));
}

}  // namespace

std::array<std::uint8_t, PAGE_SIZE> BPlusTreeEngine::encode_node(const Node& node) {
    if ((!node.leaf && node.children.size() != node.keys.size() + 1) ||
        (node.leaf && (!node.keys.empty() || !node.children.empty()))) {
        throw std::runtime_error("invalid in-memory B+ tree node");
    }

    const std::size_t count = node.leaf ? node.entries.size() : node.keys.size();
    if (count > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error("too many slots in B+ tree page");
    }

    std::array<std::uint8_t, PAGE_SIZE> page {};
    auto* header = reinterpret_cast<PageHeader*>(page.data());
    std::memcpy(header->magic, PAGE_MAGIC, sizeof(header->magic));
    header->page_id = node.id;
    header->parent_id = node.parent;
    header->next_page_id = node.leaf ? node.next : 0;
    header->prev_page_id = node.leaf ? node.prev : node.children.front();
    header->slot_count = static_cast<std::uint16_t>(count);
    header->free_start = static_cast<std::uint16_t>(PAGE_SIZE);
    header->is_leaf = node.leaf ? 1 : 0;

    auto* slots = reinterpret_cast<Slot*>(page.data() + sizeof(PageHeader));
    std::size_t cursor = PAGE_SIZE;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t record_size =
            node.leaf ? leaf_record_size(node.entries[i]) : internal_record_size(node.keys[i]);
        const std::size_t directory_end =
            sizeof(PageHeader) + (i + 1) * sizeof(Slot);
        if (record_size > cursor || cursor - record_size < directory_end) {
            throw std::length_error("B+ tree node does not fit in one page");
        }
        cursor -= record_size;
        std::uint8_t* record = page.data() + cursor;

        if (node.leaf) {
            const auto& entry = node.entries[i];
            write_u16(record, static_cast<std::uint16_t>(entry.first.size()));
            write_u16(record + 2, static_cast<std::uint16_t>(entry.second.size()));
            std::memcpy(record + 4, entry.first.data(), entry.first.size());
            std::memcpy(record + 4 + entry.first.size(), entry.second.data(), entry.second.size());
        } else {
            const std::string& key = node.keys[i];
            write_u16(record, static_cast<std::uint16_t>(key.size()));
            std::memcpy(record + 2, key.data(), key.size());
            write_u32(record + 2 + key.size(), node.children[i + 1]);
        }

        slots[i].offset = static_cast<std::uint16_t>(cursor);
        slots[i].size = static_cast<std::uint16_t>(record_size);
    }
    header->free_start = static_cast<std::uint16_t>(cursor);
    return page;
}

BPlusTreeEngine::Node BPlusTreeEngine::decode_node(
    const std::array<std::uint8_t, PAGE_SIZE>& page,
    std::uint32_t expected_id) {
    const auto* header = reinterpret_cast<const PageHeader*>(page.data());
    if (std::memcmp(header->magic, PAGE_MAGIC, sizeof(header->magic)) != 0 ||
        header->page_id != expected_id ||
        header->is_leaf > 1) {
        throw std::runtime_error("invalid B+ tree page header");
    }

    const std::size_t directory_end =
        sizeof(PageHeader) + static_cast<std::size_t>(header->slot_count) * sizeof(Slot);
    if (directory_end > PAGE_SIZE || header->free_start < directory_end ||
        header->free_start > PAGE_SIZE) {
        throw std::runtime_error("invalid B+ tree slot directory");
    }

    Node node;
    node.id = header->page_id;
    node.parent = header->parent_id;
    node.next = header->next_page_id;
    node.prev = header->prev_page_id;
    node.leaf = header->is_leaf != 0;
    if (!node.leaf) {
        if (header->prev_page_id == 0) {
            throw std::runtime_error("internal B+ tree page has no leftmost child");
        }
        node.children.push_back(header->prev_page_id);
    }

    const auto* slots = reinterpret_cast<const Slot*>(page.data() + sizeof(PageHeader));
    for (std::size_t i = 0; i < header->slot_count; ++i) {
        const Slot& slot = slots[i];
        if (slot.offset < header->free_start ||
            static_cast<std::size_t>(slot.offset) + slot.size > PAGE_SIZE) {
            throw std::runtime_error("invalid B+ tree record slot");
        }
        const std::uint8_t* record = page.data() + slot.offset;
        if (node.leaf) {
            if (slot.size < 4) {
                throw std::runtime_error("truncated B+ tree leaf record");
            }
            const std::uint16_t key_size = read_u16(record);
            const std::uint16_t value_size = read_u16(record + 2);
            if (4u + key_size + value_size != slot.size) {
                throw std::runtime_error("invalid B+ tree leaf record size");
            }
            node.entries.emplace_back(
                std::string(reinterpret_cast<const char*>(record + 4), key_size),
                std::string(reinterpret_cast<const char*>(record + 4 + key_size), value_size));
        } else {
            if (slot.size < 6) {
                throw std::runtime_error("truncated B+ tree internal record");
            }
            const std::uint16_t key_size = read_u16(record);
            if (2u + key_size + 4u != slot.size) {
                throw std::runtime_error("invalid B+ tree internal record size");
            }
            node.keys.emplace_back(reinterpret_cast<const char*>(record + 2), key_size);
            node.children.push_back(read_u32(record + 2 + key_size));
        }
    }

    const auto sorted_entries = [](const KVPair& left, const KVPair& right) {
        return left.first < right.first;
    };
    if (node.leaf && !std::is_sorted(node.entries.begin(), node.entries.end(), sorted_entries)) {
        throw std::runtime_error("unsorted B+ tree leaf page");
    }
    if (!node.leaf && !std::is_sorted(node.keys.begin(), node.keys.end())) {
        throw std::runtime_error("unsorted B+ tree internal page");
    }
    return node;
}

namespace {

std::size_t choose_leaf_split(const std::vector<KVPair>& entries) {
    std::size_t best = 0;
    std::size_t best_difference = std::numeric_limits<std::size_t>::max();
    for (std::size_t cut = 1; cut < entries.size(); ++cut) {
        const std::vector<KVPair> left(entries.begin(), entries.begin() + cut);
        const std::vector<KVPair> right(entries.begin() + cut, entries.end());
        const std::size_t left_size = leaf_used(left);
        const std::size_t right_size = leaf_used(right);
        if (left_size > PAGE_SIZE || right_size > PAGE_SIZE) {
            continue;
        }
        const std::size_t difference =
            left_size > right_size ? left_size - right_size : right_size - left_size;
        if (difference < best_difference) {
            best = cut;
            best_difference = difference;
        }
    }
    if (best == 0) {
        throw std::length_error("leaf records cannot be divided into valid pages");
    }
    return best;
}

std::size_t choose_internal_split(const std::vector<std::string>& keys) {
    std::size_t best = 0;
    std::size_t best_difference = std::numeric_limits<std::size_t>::max();
    for (std::size_t middle = 1; middle < keys.size(); ++middle) {
        const std::vector<std::string> left(keys.begin(), keys.begin() + middle);
        const std::vector<std::string> right(keys.begin() + middle + 1, keys.end());
        const std::size_t left_size = internal_used(left);
        const std::size_t right_size = internal_used(right);
        if (left_size > PAGE_SIZE || right_size > PAGE_SIZE) {
            continue;
        }
        const std::size_t difference =
            left_size > right_size ? left_size - right_size : right_size - left_size;
        if (difference < best_difference) {
            best = middle;
            best_difference = difference;
        }
    }
    if (best == 0) {
        throw std::length_error("internal records cannot be divided into valid pages");
    }
    return best;
}

}  // namespace

BPlusTreeEngine::BPlusTreeEngine(const std::string& directory)
        : dir_(directory),
          data_path_(dir_ + "/btree.pages"),
          manifest_path_(dir_ + "/manifest"),
          wal_path_(dir_ + "/wal.log") {
        ensure_directory(dir_);
        data_fd_ = ::open(data_path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (data_fd_ < 0) {
            fail("open data file");
        }
        wal_fd_ = ::open(wal_path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (wal_fd_ < 0) {
            const int saved = errno;
            close_nointr(data_fd_);
            errno = saved;
            fail("open WAL");
        }

        try {
            sync_directory(dir_);
            recover_wal();
            if (!load_manifest()) {
                initialize_empty_tree();
            }
        } catch (...) {
            close_nointr(wal_fd_);
            close_nointr(data_fd_);
            wal_fd_ = -1;
            data_fd_ = -1;
            throw;
        }
    }

BPlusTreeEngine::~BPlusTreeEngine() {
        if (data_fd_ >= 0) {
            ::fdatasync(data_fd_);
        }
        if (wal_fd_ >= 0) {
            ::fdatasync(wal_fd_);
        }
        close_nointr(wal_fd_);
        close_nointr(data_fd_);
    }

void BPlusTreeEngine::insert(const std::string& key, const std::string& value) {
        validate_record(key, value);
        begin_transaction();
        try {
            SplitResult result = insert_recursive(metadata_.root, key, value);
            if (result.split) {
                const std::uint32_t old_root = metadata_.root;
                Node root;
                root.id = allocate_page();
                root.leaf = false;
                root.keys.push_back(result.separator);
                root.children = {old_root, result.right_page};
                write_node(root);
                set_parent(old_root, root.id);
                set_parent(result.right_page, root.id);
                metadata_.root = root.id;
            }
            commit_transaction();
        } catch (...) {
            abort_transaction();
            throw;
        }
    }

void BPlusTreeEngine::remove(const std::string& key) {
        validate_key(key);
        begin_transaction();
        try {
            const std::uint32_t leaf_id = find_leaf(key);
            Node leaf = read_node(leaf_id);
            auto position = std::lower_bound(
                leaf.entries.begin(), leaf.entries.end(), key,
                [](const KVPair& entry, const std::string& needle) {
                    return entry.first < needle;
                });
            if (position != leaf.entries.end() && position->first == key) {
                leaf.entries.erase(position);
                write_node(leaf);
                maybe_merge_leaf(leaf.id);
            }
            commit_transaction();
        } catch (...) {
            abort_transaction();
            throw;
        }
    }

std::optional<std::string> BPlusTreeEngine::get(const std::string& key) const {
        validate_key(key);
        const Node leaf = read_node(find_leaf(key));
        const auto position = std::lower_bound(
            leaf.entries.begin(), leaf.entries.end(), key,
            [](const KVPair& entry, const std::string& needle) {
                return entry.first < needle;
            });
        if (position == leaf.entries.end() || position->first != key) {
            return std::nullopt;
        }
        return position->second;
    }

std::vector<KVPair> BPlusTreeEngine::rangeQuery(
    const std::string& start_key,
    const std::string& end_key) const {
        std::string start = start_key;
        std::string end = end_key;
        if (start > end) {
            std::swap(start, end);
        }
        std::vector<KVPair> result;
        std::uint32_t page_id = find_leaf(start);
        std::unordered_set<std::uint32_t> visited;
        while (page_id != 0) {
            if (!visited.insert(page_id).second || visited.size() > metadata_.highest) {
                throw std::runtime_error("cycle in B+ tree leaf chain");
            }
            const Node leaf = read_node(page_id);
            if (!leaf.leaf) {
                throw std::runtime_error("leaf chain references an internal page");
            }
            for (const KVPair& entry : leaf.entries) {
                if (entry.first < start) {
                    continue;
                }
                if (entry.first > end) {
                    return result;
                }
                result.push_back(entry);
            }
            page_id = leaf.next;
        }
        return result;
    }

std::size_t BPlusTreeEngine::key_count() const {
        std::size_t count = 0;
        std::uint32_t page_id = leftmost_leaf();
        std::unordered_set<std::uint32_t> visited;
        while (page_id != 0) {
            if (!visited.insert(page_id).second || visited.size() > metadata_.highest) {
                throw std::runtime_error("cycle in B+ tree leaf chain");
            }
            const Node leaf = read_node(page_id);
            count += leaf.entries.size();
            page_id = leaf.next;
        }
        return count;
    }

std::uint32_t BPlusTreeEngine::height() const {
        std::uint32_t result = 0;
        std::uint32_t page_id = metadata_.root;
        std::unordered_set<std::uint32_t> visited;
        while (page_id != 0) {
            if (!visited.insert(page_id).second || visited.size() > metadata_.highest) {
                throw std::runtime_error("cycle in B+ tree hierarchy");
            }
            ++result;
            const Node node = read_node(page_id);
            if (node.leaf) {
                return result;
            }
            page_id = node.children.front();
        }
        return 0;
    }

bool BPlusTreeEngine::flush() {
        sync_fd(data_fd_);
        sync_fd(wal_fd_);
        return true;
    }

std::string& BPlusTreeEngine::active_file_path() {
        return data_path_;
    }

void BPlusTreeEngine::validate_key(const std::string& key) {
        if (key.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::length_error("B+ tree key is too large");
        }
    }

void BPlusTreeEngine::validate_record(
    const std::string& key,
    const std::string& value) {
        validate_key(key);
        if (value.size() > std::numeric_limits<std::uint16_t>::max() ||
            sizeof(PageHeader) + sizeof(Slot) + 4 + key.size() + value.size() > PAGE_SIZE) {
            throw std::length_error("B+ tree key/value record is too large for one page");
        }
        if (sizeof(PageHeader) + sizeof(Slot) + internal_record_size(key) > PAGE_SIZE) {
            throw std::length_error("B+ tree key is too large for an internal page");
        }
    }

void BPlusTreeEngine::initialize_empty_tree() {
        if (::ftruncate(data_fd_, 0) < 0) {
            fail("ftruncate data file");
        }
        metadata_ = Metadata{1, 1};
        Node root;
        root.id = 1;
        root.leaf = true;
        const auto page = encode_node(root);
        pwrite_all(data_fd_, page.data(), page.size(), page_offset(1));
        sync_fd(data_fd_);
        persist_manifest(metadata_);
        truncate_wal();
    }

bool BPlusTreeEngine::load_manifest() {
        const int fd = ::open(manifest_path_.c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) {
                return false;
            }
            fail("open manifest");
        }
        ManifestDisk disk {};
        const bool complete = read_all_or_eof(fd, &disk, sizeof(disk));
        close_nointr(fd);
        if (!complete || std::memcmp(disk.magic, MANIFEST_MAGIC, sizeof(disk.magic)) != 0 ||
            disk.page_size != PAGE_SIZE ||
            disk.root_page_id == 0 || disk.highest_page_id < disk.root_page_id) {
            throw std::runtime_error("invalid or truncated B+ tree manifest");
        }
        metadata_ = Metadata{disk.root_page_id, disk.highest_page_id};
        return true;
    }

void BPlusTreeEngine::persist_manifest(const Metadata& metadata) {
        ManifestDisk disk {};
        std::memcpy(disk.magic, MANIFEST_MAGIC, sizeof(disk.magic));
        disk.page_size = PAGE_SIZE;
        disk.root_page_id = metadata.root;
        disk.highest_page_id = metadata.highest;

        const std::string temporary = manifest_path_ + ".tmp";
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fail("open temporary manifest");
        }
        try {
            write_all(fd, &disk, sizeof(disk));
            sync_fd(fd);
        } catch (...) {
            close_nointr(fd);
            ::unlink(temporary.c_str());
            throw;
        }
        close_nointr(fd);
        if (::rename(temporary.c_str(), manifest_path_.c_str()) < 0) {
            ::unlink(temporary.c_str());
            fail("rename manifest");
        }
        sync_directory(dir_);
    }

void BPlusTreeEngine::begin_transaction() {
        if (transaction_active_) {
            throw std::logic_error("nested B+ tree transaction");
        }
        if (::ftruncate(wal_fd_, 0) < 0 ||
            ::lseek(wal_fd_, 0, SEEK_SET) == static_cast<off_t>(-1)) {
            fail("reset WAL");
        }
        WalHeader header {};
        std::memcpy(header.magic, WAL_MAGIC, sizeof(header.magic));
        header.version = FORMAT_VERSION;
        header.old_root_page_id = metadata_.root;
        header.old_highest_page_id = metadata_.highest;
        write_all(wal_fd_, &header, sizeof(header));
        sync_fd(wal_fd_);
        transaction_start_ = metadata_;
        undo_logged_.clear();
        transaction_active_ = true;
    }

void BPlusTreeEngine::log_undo_page(std::uint32_t page_id) {
        if (!transaction_active_ || page_id > transaction_start_.highest ||
            !undo_logged_.insert(page_id).second) {
            return;
        }
        std::array<std::uint8_t, PAGE_SIZE> old_page {};
        pread_all(data_fd_, old_page.data(), old_page.size(), page_offset(page_id));
        if (::lseek(wal_fd_, 0, SEEK_END) == static_cast<off_t>(-1)) {
            fail("seek WAL");
        }
        const WalRecordHeader record {1, page_id, static_cast<std::uint32_t>(PAGE_SIZE)};
        write_all(wal_fd_, &record, sizeof(record));
        write_all(wal_fd_, old_page.data(), old_page.size());
        sync_fd(wal_fd_);
    }

void BPlusTreeEngine::commit_transaction() {
        sync_fd(data_fd_);
        persist_manifest(metadata_);
        if (::lseek(wal_fd_, 0, SEEK_END) == static_cast<off_t>(-1)) {
            fail("seek WAL");
        }
        const WalRecordHeader commit {2, 0, 0};
        write_all(wal_fd_, &commit, sizeof(commit));
        sync_fd(wal_fd_);
        truncate_wal();
        transaction_active_ = false;
        undo_logged_.clear();
    }

void BPlusTreeEngine::abort_transaction() noexcept {
        if (!transaction_active_) {
            return;
        }
        transaction_active_ = false;
        undo_logged_.clear();
        try {
            recover_wal();
            if (!load_manifest()) {
                metadata_ = transaction_start_;
            }
        } catch (...) {
           
        }
    }

void BPlusTreeEngine::recover_wal() {
        if (::lseek(wal_fd_, 0, SEEK_SET) == static_cast<off_t>(-1)) {
            fail("seek WAL");
        }
        struct stat info {};
        if (::fstat(wal_fd_, &info) < 0) {
            fail("stat WAL");
        }
        if (info.st_size == 0) {
            return;
        }

        WalHeader header {};
        if (!read_all_or_eof(wal_fd_, &header, sizeof(header)) ||
            std::memcmp(header.magic, WAL_MAGIC, sizeof(header.magic)) != 0 ||
            header.version != FORMAT_VERSION) {
            // A torn initial WAL cannot precede a page write because begin_transaction syncs it.
            truncate_wal();
            return;
        }

        std::vector<std::pair<std::uint32_t, std::array<std::uint8_t, PAGE_SIZE>>> undo;
        bool committed = false;
        for (;;) {
            WalRecordHeader record {};
            if (!read_all_or_eof(wal_fd_, &record, sizeof(record))) {
                break;
            }
            if (record.type == 2 && record.page_id == 0 && record.size == 0) {
                committed = true;
                break;
            }
            if (record.type != 1 || record.page_id == 0 || record.size != PAGE_SIZE) {
                break;
            }
            std::array<std::uint8_t, PAGE_SIZE> page {};
            if (!read_all_or_eof(wal_fd_, page.data(), page.size())) {
                break;
            }
            undo.emplace_back(record.page_id, std::move(page));
        }

        if (!committed) {
            for (const auto& item : undo) {
                pwrite_all(
                    data_fd_, item.second.data(), item.second.size(), page_offset(item.first));
            }
            const off_t old_size =
                static_cast<off_t>(header.old_highest_page_id) * static_cast<off_t>(PAGE_SIZE);
            if (::ftruncate(data_fd_, old_size) < 0) {
                fail("rollback data file");
            }
            sync_fd(data_fd_);
            if (header.old_root_page_id != 0 && header.old_highest_page_id != 0) {
                persist_manifest(Metadata{
                    header.old_root_page_id,
                    header.old_highest_page_id});
            }
        }
        truncate_wal();
    }

void BPlusTreeEngine::truncate_wal() {
        if (::ftruncate(wal_fd_, 0) < 0 ||
            ::lseek(wal_fd_, 0, SEEK_SET) == static_cast<off_t>(-1)) {
            fail("truncate WAL");
        }
        sync_fd(wal_fd_);
    }

BPlusTreeEngine::Node BPlusTreeEngine::read_node(std::uint32_t page_id) const {
        if (page_id == 0 || page_id > metadata_.highest) {
            throw std::runtime_error("B+ tree page reference is out of range");
        }
        std::array<std::uint8_t, PAGE_SIZE> page {};
        pread_all(data_fd_, page.data(), page.size(), page_offset(page_id));
        return decode_node(page, page_id);
    }

void BPlusTreeEngine::write_node(const Node& node) {
        const auto page = encode_node(node);
        log_undo_page(node.id);
        pwrite_all(data_fd_, page.data(), page.size(), page_offset(node.id));
    }

std::uint32_t BPlusTreeEngine::allocate_page() {
        if (metadata_.highest == std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("B+ tree page id space exhausted");
        }
        return ++metadata_.highest;
    }

void BPlusTreeEngine::set_parent(std::uint32_t page_id, std::uint32_t parent_id) {
        Node child = read_node(page_id);
        if (child.parent != parent_id) {
            child.parent = parent_id;
            write_node(child);
        }
    }

std::uint32_t BPlusTreeEngine::find_leaf(const std::string& key) const {
        std::uint32_t page_id = metadata_.root;
        std::unordered_set<std::uint32_t> visited;
        for (;;) {
            if (!visited.insert(page_id).second || visited.size() > metadata_.highest) {
                throw std::runtime_error("cycle in B+ tree hierarchy");
            }
            const Node node = read_node(page_id);
            if (node.leaf) {
                return page_id;
            }
            const auto position = std::upper_bound(node.keys.begin(), node.keys.end(), key);
            page_id = node.children[static_cast<std::size_t>(position - node.keys.begin())];
        }
    }

std::uint32_t BPlusTreeEngine::leftmost_leaf() const {
        std::uint32_t page_id = metadata_.root;
        std::unordered_set<std::uint32_t> visited;
        for (;;) {
            if (!visited.insert(page_id).second || visited.size() > metadata_.highest) {
                throw std::runtime_error("cycle in B+ tree hierarchy");
            }
            const Node node = read_node(page_id);
            if (node.leaf) {
                return page_id;
            }
            page_id = node.children.front();
        }
    }

BPlusTreeEngine::SplitResult BPlusTreeEngine::insert_recursive(
        std::uint32_t page_id,
        const std::string& key,
        const std::string& value) {
        Node node = read_node(page_id);
        if (node.leaf) {
            auto position = std::lower_bound(
                node.entries.begin(), node.entries.end(), key,
                [](const KVPair& entry, const std::string& needle) {
                    return entry.first < needle;
                });
            if (position != node.entries.end() && position->first == key) {
                position->second = value;
            } else {
                node.entries.insert(position, KVPair{key, value});
            }

            const std::size_t used = leaf_used(node.entries);
            if (used <= PAGE_SIZE &&
                (occupancy(used) <= SPLIT_OCCUPANCY || node.entries.size() < 2)) {
                write_node(node);
                return {};
            }

            const std::size_t cut = choose_leaf_split(node.entries);
            Node right;
            right.id = allocate_page();
            right.parent = node.parent;
            right.leaf = true;
            right.prev = node.id;
            right.next = node.next;
            right.entries.assign(node.entries.begin() + cut, node.entries.end());
            node.entries.erase(node.entries.begin() + cut, node.entries.end());
            node.next = right.id;

            if (right.next != 0) {
                Node successor = read_node(right.next);
                successor.prev = right.id;
                write_node(successor);
            }
            write_node(node);
            write_node(right);
            return SplitResult{true, right.entries.front().first, right.id};
        }

        const auto child_position = std::upper_bound(node.keys.begin(), node.keys.end(), key);
        const std::size_t child_index =
            static_cast<std::size_t>(child_position - node.keys.begin());
        SplitResult child_split = insert_recursive(node.children[child_index], key, value);
        if (!child_split.split) {
            return {};
        }

        node.keys.insert(node.keys.begin() + child_index, child_split.separator);
        node.children.insert(node.children.begin() + child_index + 1, child_split.right_page);
        set_parent(child_split.right_page, node.id);

        const std::size_t used = internal_used(node.keys);
        if (used <= PAGE_SIZE &&
            (occupancy(used) <= SPLIT_OCCUPANCY || node.keys.size() < 3)) {
            write_node(node);
            return {};
        }

        const std::size_t middle = choose_internal_split(node.keys);
        const std::string separator = node.keys[middle];
        Node right;
        right.id = allocate_page();
        right.parent = node.parent;
        right.leaf = false;
        right.keys.assign(node.keys.begin() + middle + 1, node.keys.end());
        right.children.assign(node.children.begin() + middle + 1, node.children.end());
        node.keys.erase(node.keys.begin() + middle, node.keys.end());
        node.children.erase(node.children.begin() + middle + 1, node.children.end());

        write_node(node);
        write_node(right);
        for (std::uint32_t child : right.children) {
            set_parent(child, right.id);
        }
        return SplitResult{true, separator, right.id};
    }

void BPlusTreeEngine::maybe_merge_leaf(std::uint32_t leaf_id) {
        Node leaf = read_node(leaf_id);
        if (!leaf.leaf || leaf.parent == 0 ||
            occupancy(leaf_used(leaf.entries)) >= MERGE_OCCUPANCY ||
            leaf.next == 0) {
            return;
        }

        Node right = read_node(leaf.next);
        if (!right.leaf || right.parent != leaf.parent) {
            return;
        }
        std::vector<KVPair> combined = leaf.entries;
        combined.insert(combined.end(), right.entries.begin(), right.entries.end());
        if (leaf_used(combined) > PAGE_SIZE) {
            return;
        }

        Node parent = read_node(leaf.parent);
        const auto child = std::find(parent.children.begin(), parent.children.end(), right.id);
        if (child == parent.children.end()) {
            throw std::runtime_error("leaf sibling is missing from its parent");
        }
        const std::size_t child_index =
            static_cast<std::size_t>(child - parent.children.begin());
        if (child_index == 0) {
            throw std::runtime_error("invalid right-sibling position");
        }

        leaf.entries = std::move(combined);
        leaf.next = right.next;
        if (leaf.next != 0) {
            Node successor = read_node(leaf.next);
            successor.prev = leaf.id;
            write_node(successor);
        }
        parent.children.erase(parent.children.begin() + child_index);
        parent.keys.erase(parent.keys.begin() + child_index - 1);
        write_node(leaf);

        if (parent.id == metadata_.root && parent.keys.empty()) {
            leaf.parent = 0;
            write_node(leaf);
            metadata_.root = leaf.id;
        } else {
            write_node(parent);
        }
    }

std::vector<KVPair> BPlusTreeEngine::range_scan(
    const std::string& start_key,
    const std::string& end_key) const {
    return rangeQuery(start_key, end_key);
}

void BPlusTreeEngine::force_compact() {
    flush();
}
