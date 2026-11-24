#pragma once
#include <iostream>
#include <string>

namespace testutil {

inline int g_failures = 0;

inline void check(bool cond, const std::string& name) {
    if (cond) {
        std::cout << "[PASS] " << name << std::endl;
    } else {
        std::cout << "[FAIL] " << name << std::endl;
        ++g_failures;
    }
}

inline void eq(const std::string& actual, const std::string& expected, const std::string& name) {
    check(actual == expected, name + " (actual=" + actual + ", expected=" + expected + ")");
}

inline int finish(const std::string& suite) {
    if (g_failures == 0) {
        std::cout << "=== " << suite << " ALL PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << suite << " FAILED: " << g_failures << " ===" << std::endl;
    return 1;
}

} // namespace testutil
