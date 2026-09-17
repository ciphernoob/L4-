#include "Test.h"

#include "net/Buffer.h"

#include <array>
#include <string>

using l4lb::net::Buffer;

L4LB_TEST(BufferSupportsPartialAndExactRetrieval) {
    Buffer buffer;
    buffer.Append("abcdef", 6);
    L4LB_REQUIRE(buffer.RetrieveAsString(2) == "ab");
    L4LB_REQUIRE(buffer.ReadableBytes() == 4);
    L4LB_REQUIRE(buffer.RetrieveAsString(4) == "cdef");
    L4LB_REQUIRE(buffer.ReadableBytes() == 0);
}

L4LB_TEST(BufferPreservesBinaryZerosAndGrows) {
    Buffer buffer(2);
    const std::array<char, 6> bytes{{'a', '\0', 'b', 'c', '\0', 'd'}};
    buffer.Append(bytes.data(), bytes.size());
    const std::string result = buffer.RetrieveAllAsString();
    L4LB_REQUIRE(result.size() == bytes.size());
    L4LB_REQUIRE(result[1] == '\0');
    L4LB_REQUIRE(result[4] == '\0');
}

L4LB_TEST(BufferCompactsConsumedSpace) {
    Buffer buffer(8);
    buffer.Append("123456", 6);
    buffer.Retrieve(4);
    buffer.Append("abcdef", 6);
    L4LB_REQUIRE(buffer.RetrieveAllAsString() == "56abcdef");
}

