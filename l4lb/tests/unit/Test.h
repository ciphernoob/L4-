#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace l4lb {
namespace test {

using TestFunction = std::function<void()>;
using TestCase = std::pair<std::string, TestFunction>;

std::vector<TestCase>& Registry();

class Registrar {
public:
    Registrar(const char* name, TestFunction function);
};

inline void Require(bool condition, const char* expression) {
    if (!condition) {
        throw std::runtime_error(std::string("requirement failed: ") + expression);
    }
}

}  // namespace test
}  // namespace l4lb

#define L4LB_TEST(name)                                      \
    void name();                                             \
    static ::l4lb::test::Registrar name##_registrar(#name, name); \
    void name()

#define L4LB_REQUIRE(expression) ::l4lb::test::Require((expression), #expression)

