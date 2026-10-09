#ifndef BPLUS_TREE_ENGINE_HPP
#define BPLUS_TREE_ENGINE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using KVPair = std::pair<std::string, std::string>;

class BPlusTreeEngine {
public:
    explicit BPlusTreeEngine(const std::string& dir_path);
    ~BPlusTreeEngine();

    BPlusTreeEngine(const BPlusTreeEngine&) = delete;
    BPlusTreeEngine& operator=(const BPlusTreeEngine&) = delete;

    void insert(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;
    std::vector<KVPair> rangeQuery(
        const std::string& start_key,
        const std::string& end_key) const;

    bool flush();
    void force_compact();

    std::size_t key_count() const;
    std::uint32_t height() const;
    std::string& active_file_path();

    std::vector<KVPair> range_scan(
        const std::string& start_key,
        const std::string& end_key) const;

private:
    struct Metadata {
        std::uint32_t root = 0;
        std::uint32_t highest = 0;
    };

    struct Node {
        std::uint32_t id = 0;
        std::uint32_t parent = 0;
        std::uint32_t next = 0;
        std::uint32_t prev = 0;
        bool leaf = true;
        std::vector<KVPair> entries;
        std::vector<std::string> keys;
        std::vector<std::uint32_t> children;
    };

    struct SplitResult {
        bool split = false;
        std::string separator;
        std::uint32_t right_page = 0;
    };

    std::string dir_;
    std::string data_path_;
    std::string manifest_path_;
    std::string wal_path_;
    int data_fd_ = -1;
    int wal_fd_ = -1;
    Metadata metadata_;
    bool transaction_active_ = false;
    Metadata transaction_start_;
    std::unordered_set<std::uint32_t> undo_logged_;

    static std::array<std::uint8_t, 4096> encode_node(const Node& node);
    static Node decode_node(
        const std::array<std::uint8_t, 4096>& page,
        std::uint32_t expected_id);
    static void validate_key(const std::string& key);
    static void validate_record(const std::string& key, const std::string& value);

    void initialize_empty_tree();
    bool load_manifest();
    void persist_manifest(const Metadata& metadata);
    void begin_transaction();
    void log_undo_page(std::uint32_t page_id);
    void commit_transaction();
    void abort_transaction() noexcept;
    void recover_wal();
    void truncate_wal();

    Node read_node(std::uint32_t page_id) const;
    void write_node(const Node& node);
    std::uint32_t allocate_page();
    void set_parent(std::uint32_t page_id, std::uint32_t parent_id);
    std::uint32_t find_leaf(const std::string& key) const;
    std::uint32_t leftmost_leaf() const;
    SplitResult insert_recursive(
        std::uint32_t page_id,
        const std::string& key,
        const std::string& value);
    void maybe_merge_leaf(std::uint32_t leaf_id);
};

#endif
