#include "test_util.hpp"

#include <iostream>
#include <string>

#include "edgegate/buffer.hpp"

using eg::Buffer;

namespace {

void test_append_consume() {
    Buffer b;
    b.append("hello ");
    b.append("world");
    REQUIRE_EQ(b.size(), 11u);
    REQUIRE_EQ(std::string(b.view()), "hello world");
    b.consume(6);
    REQUIRE_EQ(std::string(b.view()), "world");
    b.consume(5);
    REQUIRE(b.empty());
    REQUIRE_EQ(b.size(), 0u);
}

void test_reserve_and_compact() {
    Buffer b;
    b.append(std::string(1000, 'a'));
    b.consume(900);
    REQUIRE_EQ(b.size(), 100u);
    b.append(std::string(5000, 'b'));
    REQUIRE_EQ(b.size(), 5100u);
    std::string v = std::string(b.view());
    REQUIRE_EQ(v.substr(0, 100), std::string(100, 'a'));
    REQUIRE_EQ(v.substr(100), std::string(5000, 'b'));
    b.compact();
    REQUIRE_EQ(b.size(), 5100u);
    REQUIRE_EQ(std::string(b.view()).substr(0, 3), "aaa");
}

void test_binary_data() {
    Buffer b;
    const std::string raw("\0\1\2\3", 4);
    b.append(raw.data(), raw.size());
    REQUIRE_EQ(b.size(), 4u);
    REQUIRE_EQ(std::string(b.data(), b.size()), raw);
}

void test_write_pointer() {
    Buffer b;
    b.reserve(10);
    char* p = b.write_ptr();
    p[0] = 'x';
    p[1] = 'y';
    b.commit(2);
    REQUIRE_EQ(std::string(b.view()), "xy");
    REQUIRE_EQ(b.writable() >= 8, true);
}

}  // namespace

int main() {
    try {
        test_append_consume();
        test_reserve_and_compact();
        test_binary_data();
        test_write_pointer();
    } catch (const std::exception& e) {
        std::cerr << "buffer_test failed: " << e.what() << "\n";
        return 1;
    }
    std::cout << "buffer_test: all checks passed\n";
    return 0;
}
