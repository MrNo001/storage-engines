#include "b+tree-engine.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

std::string key_for(int value) {
    std::ostringstream key;
    key << "key-" << std::setfill('0') << std::setw(6) << value;
    return key.str();
}

void write_all(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size != 0) {
        const ssize_t count = ::write(fd, bytes, size);
        assert(count > 0);
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
}

#pragma pack(push, 1)
struct TestWalHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t old_root;
    std::uint32_t old_highest;
};

struct TestWalRecord {
    std::uint8_t type;
    std::uint32_t page_id;
    std::uint32_t size;
};
#pragma pack(pop)

void test_incomplete_transaction_recovery() {
    const std::filesystem::path directory = "btree_recovery_store";
    std::filesystem::remove_all(directory);
    {
        BPlusTreeEngine tree(directory.string());
        tree.insert("durable", "value");
    }

    std::array<std::uint8_t, 4096> old_page {};
    const std::string data_path = (directory / "btree.pages").string();
    int data_fd = ::open(data_path.c_str(), O_RDWR);
    assert(data_fd >= 0);
    assert(::pread(data_fd, old_page.data(), old_page.size(), 0) ==
           static_cast<ssize_t>(old_page.size()));

    const std::string wal_path = (directory / "wal.log").string();
    int wal_fd = ::open(wal_path.c_str(), O_WRONLY | O_TRUNC);
    assert(wal_fd >= 0);
    TestWalHeader header {};
    std::memcpy(header.magic, "BTWAL01", 7);
    header.version = 1;
    header.old_root = 1;
    header.old_highest = 1;
    const TestWalRecord record {1, 1, 4096};
    write_all(wal_fd, &header, sizeof(header));
    write_all(wal_fd, &record, sizeof(record));
    write_all(wal_fd, old_page.data(), old_page.size());
    assert(::fdatasync(wal_fd) == 0);
    assert(::close(wal_fd) == 0);

    std::array<std::uint8_t, 4096> damaged_page {};
    assert(::pwrite(data_fd, damaged_page.data(), damaged_page.size(), 0) ==
           static_cast<ssize_t>(damaged_page.size()));
    assert(::fdatasync(data_fd) == 0);
    assert(::close(data_fd) == 0);

    {
        BPlusTreeEngine recovered(directory.string());
        assert(recovered.get("durable").value() == "value");
        assert(std::filesystem::file_size(directory / "wal.log") == 0);
    }
    std::filesystem::remove_all(directory);
}

void test_operations_and_reopen() {
    const std::filesystem::path directory = "btree_test_store";
    std::filesystem::remove_all(directory);

    {
        BPlusTreeEngine tree(directory.string());
        tree.insert("", "empty-key");
        for (int i = 0; i < 150; ++i) {
            tree.insert(key_for(i), "value-" + std::to_string(i) + std::string(80, 'x'));
        }
        tree.insert(key_for(100), "updated");

        assert(tree.get("").value() == "empty-key");
        assert(tree.get(key_for(100)).value() == "updated");
        assert(!tree.get("missing").has_value());
        assert(tree.key_count() == 151);
        assert(tree.height() >= 2);

        const auto range = tree.rangeQuery(key_for(50), key_for(99));
        assert(range.size() == 50);
        assert(range.front().first == key_for(50));
        assert(range.back().first == key_for(99));

        for (int i = 0; i < 150; i += 3) {
            tree.remove(key_for(i));
        }
        tree.remove("missing");
        assert(tree.flush());
    }

    {
        BPlusTreeEngine tree(directory.string());
        assert(tree.get("").value() == "empty-key");
        for (int i = 0; i < 150; ++i) {
            const auto value = tree.get(key_for(i));
            if (i % 3 == 0) {
                assert(!value.has_value());
            } else {
                assert(value.has_value());
            }
        }
        assert(tree.key_count() == 101);

        const auto reversed = tree.range_scan(key_for(99), key_for(50));
        assert(reversed.size() == 33);
    }

    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    test_operations_and_reopen();
    test_incomplete_transaction_recovery();
}
