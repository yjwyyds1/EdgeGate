#pragma once

#include <stdexcept>
#include <string>

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition))                                                      \
            throw std::runtime_error(std::string("failed: ") + #condition);    \
    } while (false)

#define REQUIRE_EQ(a, b)                                                       \
    do {                                                                       \
        const auto lhs_ = (a);                                                  \
        const auto rhs_ = (b);                                                  \
        if (!(lhs_ == rhs_)) {                                                  \
            throw std::runtime_error(std::string("not equal: ") + #a + " vs " + #b); \
        }                                                                       \
    } while (false)
