#include "Test.h"

#include <exception>
#include <iostream>

namespace l4lb {
namespace test {

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(const char* name, TestFunction function) {
    Registry().emplace_back(name, std::move(function));
}

}  // namespace test
}  // namespace l4lb

int main() {
    int failures = 0;
    for (const auto& test : l4lb::test::Registry()) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}

