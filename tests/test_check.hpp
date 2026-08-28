#pragma once

#include <iostream>
#include <stdexcept>
#include <string>

namespace test_check {

inline void require(
    bool condition,
    const char* expression,
    const char* file,
    int line) {
    if (condition) {
        return;
    }

    std::cerr << "CHECK failed at " << file << ':' << line
              << ": " << expression << '\n';
    throw std::runtime_error(
        std::string("test check failed: ") + expression);
}

}  // namespace test_check

#define CHECK(expr) \
    ::test_check::require(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
