#include "lsm-engine.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::streamoff SST_FOOTER_SIZE = 32;
constexpr uint32_t SST_MAGIC = 0x4C534D31;

template <typename T>
T read_number(std::ifstream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error("unexpected end of file");
    }
    return value;
}

std::string read_string(std::ifstream& input, uint32_t size) {
    std::string value(size, '\0');
    input.read(value.data(), static_cast<std::streamsize>(size));
    if (!input) {
        throw std::runtime_error("unexpected end of file");
    }
    return value;
}

std::vector<uint64_t> print_manifest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }

    uint32_t table_count = read_number<uint32_t>(input);
    std::vector<uint64_t> table_ids;
    table_ids.reserve(table_count);

    std::cout << "\nMANIFEST: " << path << "\n"
              << "  uint32 table_count = " << table_count << "\n";
    for (uint32_t i = 0; i < table_count; ++i) {
        uint64_t id = read_number<uint64_t>(input);
        table_ids.push_back(id);
        std::cout << "  uint64 table_id[" << i << "] = " << id << "\n";
    }
    std::cout << "  file_size = " << std::filesystem::file_size(path) << " bytes\n";
    return table_ids;
}

void print_sstable(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }

    input.seekg(-SST_FOOTER_SIZE, std::ios::end);
    uint64_t bloom_offset = read_number<uint64_t>(input);
    uint64_t index_offset = read_number<uint64_t>(input);
    uint64_t data_end = read_number<uint64_t>(input);
    uint32_t record_count = read_number<uint32_t>(input);
    uint32_t magic = read_number<uint32_t>(input);

    input.seekg(static_cast<std::streamoff>(bloom_offset));
    uint32_t bloom_hash_count = read_number<uint32_t>(input);
    uint32_t bloom_bytes = read_number<uint32_t>(input);

    input.seekg(static_cast<std::streamoff>(index_offset));
    uint32_t sparse_entry_count = read_number<uint32_t>(input);

    std::cout << "\nSSTABLE: " << path << "\n"
              << "  file_size = " << std::filesystem::file_size(path) << " bytes\n"
              << "  data region       [0, " << data_end << ")\n"
              << "  bloom filter      offset=" << bloom_offset
              << ", bytes=" << bloom_bytes
              << ", hash_functions=" << bloom_hash_count << "\n"
              << "  sparse index      offset=" << index_offset
              << ", entries=" << sparse_entry_count << "\n"
              << "  footer            bytes=" << SST_FOOTER_SIZE
              << ", records=" << record_count << "\n"
              << "  magic             0x" << std::hex << std::uppercase << magic
              << std::dec << (magic == SST_MAGIC ? " (valid)" : " (INVALID)") << "\n"
              << "  first records:\n";

    input.seekg(0);
    for (uint32_t i = 0; i < record_count && i < 5; ++i) {
        uint32_t key_size = read_number<uint32_t>(input);
        uint32_t value_size = read_number<uint32_t>(input);
        uint8_t deleted = read_number<uint8_t>(input);
        std::string key = read_string(input, key_size);
        std::string value = read_string(input, value_size);

        constexpr size_t preview_size = 32;
        if (value.size() > preview_size) {
            value.resize(preview_size);
            value += "...";
        }
        std::cout << "    [" << i << "] key=\"" << key
                  << "\", value=\"" << value
                  << "\", deleted=" << static_cast<int>(deleted) << "\n";
    }
}

std::string sstable_name(uint64_t id) {
    std::ostringstream name;
    name << "sst-" << std::setfill('0') << std::setw(6) << id << ".sst";
    return name.str();
}

void test_compaction_and_range_query(const std::filesystem::path& directory) {
    constexpr int key_count = 1000;

    std::filesystem::remove_all(directory);

    {
        LsmEngine database(directory.string());
        for (int i = 0; i < key_count; ++i) {
            std::ostringstream key;
            key << "key-" << std::setfill('0') << std::setw(6) << i;
            std::string value =
                "value-for-" + key.str() + "-" +
                std::string(120, static_cast<char>('a' + (i % 26)));
            database.insert(key.str(), value);
        }

        // Add obsolete versions and tombstones so compaction has work to do.
        for (int i = 0; i < 100; ++i) {
            std::ostringstream key;
            key << "key-" << std::setfill('0') << std::setw(6) << i;
            database.insert(key.str(), "updated-" + std::to_string(i));
        }
        for (int i = 0; i < key_count; i += 10) {
            std::ostringstream key;
            key << "key-" << std::setfill('0') << std::setw(6) << i;
            database.remove(key.str());
        }

        database.force_compact();
        std::cout << "Generated " << database.key_count()
                  << " live keys in " << directory << "\n";

        auto range = database.rangeQuery("key-000100", "key-000110");
        std::cout << "rangeQuery[key-000100, key-000110] size=" << range.size()
                  << "\n";
        if (range.empty() || range.front().first < "key-000100" ||
            range.back().first > "key-000110") {
            throw std::runtime_error("rangeQuery bounds check failed");
        }
    }
}

void print_layout(const std::filesystem::path& directory) {
    std::vector<uint64_t> table_ids = print_manifest(directory / "manifest");
    for (uint64_t id : table_ids) {
        print_sstable(directory / sstable_name(id));
    }

    std::cout << "\nFiles were intentionally left in " << directory
              << " so they can be inspected with xxd or a hex editor.\n";
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path directory =
        argc > 1 ? argv[1] : "lsm_layout_store";
    test_compaction_and_range_query(directory);
    print_layout(directory);
    return 0;
}
