#include "hash_engine.hpp"

#include <iostream>

int main() {
    HashEngine he("db");
    he.insert("hejj", "ojjj");
    he.insert("hejj", "newer");
    he.insert("other", "x");

    auto v = he.get("hejj");
    if (v) {
        std::cout << *v << "\n";
    }

    he.remove("hejj");
    if (!he.get("hejj")) {
        std::cout << "removed\n";
    }

    he.insert("a", "1");
    he.insert("m", "2");
    he.insert("z", "3");
    he.insert("m", "22");
    he.remove("z");
    for (const auto& [k, v] : he.rangeQuery("a", "z")) {
        std::cout << k << "=" << v << "\n";
    }

    he.force_compact();
    std::cout << "after compact: keys=" << he.key_count()
              << " m=" << he.get("m").value_or("<missing>") << "\n";
}
