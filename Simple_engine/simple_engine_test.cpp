#include "simple_engine.hpp"

#include <iostream>
#include <string>

int main() {
    SimpleEngine se("db");
    se.insert("hejj", "ojjj");
    se.insert("hejj", "newer");
    se.insert("other", "x");

    std::string v = se.get("hejj");
    
    if (v) {
        std::cout << *v << "\n";
    }

    se.remove("hejj");
    if (!se.get("hejj")) {
        std::cout << "removed\n";
    }

    se.insert("a", "1");
    se.insert("m", "2");
    se.insert("z", "3");
    se.insert("m", "22");
    se.remove("z");
    for (const auto& [k, v] : se.rangeQuery("a", "z")) {
        std::cout << k << "=" << v << "\n";
    }
}
