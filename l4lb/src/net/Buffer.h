#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace l4lb {
namespace net {

class Buffer {
public:
    static constexpr std::size_t kCheapPrepend = 8;
    static constexpr std::size_t kInitialSize = 1024;

    explicit Buffer(std::size_t initial_size = kInitialSize);

    std::size_t ReadableBytes() const noexcept;
    std::size_t WritableBytes() const noexcept;
    std::size_t PrependableBytes() const noexcept;

    const char* Peek() const noexcept;
    char* BeginWrite() noexcept;
    const char* BeginWrite() const noexcept;

    void Retrieve(std::size_t length);
    void RetrieveUntil(const char* end);
    void RetrieveAll() noexcept;
    std::string RetrieveAsString(std::size_t length);
    std::string RetrieveAllAsString();

    void Append(const void* data, std::size_t length);
    void Append(const char* data, std::size_t length);
    void Append(const std::string& data);
    void EnsureWritableBytes(std::size_t length);
    void HasWritten(std::size_t length);

private:
    char* Begin() noexcept;
    const char* Begin() const noexcept;
    void MakeSpace(std::size_t length);

    std::vector<char> storage_;
    std::size_t reader_index_{kCheapPrepend};
    std::size_t writer_index_{kCheapPrepend};
};

}  // namespace net
}  // namespace l4lb

