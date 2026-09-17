#include "net/Buffer.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace l4lb {
namespace net {

Buffer::Buffer(std::size_t initial_size)
    : storage_(kCheapPrepend + initial_size) {}

std::size_t Buffer::ReadableBytes() const noexcept {
    return writer_index_ - reader_index_;
}

std::size_t Buffer::WritableBytes() const noexcept {
    return storage_.size() - writer_index_;
}

std::size_t Buffer::PrependableBytes() const noexcept {
    return reader_index_;
}

const char* Buffer::Peek() const noexcept {
    return Begin() + reader_index_;
}

char* Buffer::BeginWrite() noexcept {
    return Begin() + writer_index_;
}

const char* Buffer::BeginWrite() const noexcept {
    return Begin() + writer_index_;
}

void Buffer::Retrieve(std::size_t length) {
    if (length > ReadableBytes()) {
        throw std::out_of_range("Buffer::Retrieve exceeds readable bytes");
    }
    if (length == ReadableBytes()) {
        RetrieveAll();
        return;
    }
    reader_index_ += length;
}

void Buffer::RetrieveUntil(const char* end) {
    if (end < Peek() || end > BeginWrite()) {
        throw std::out_of_range("Buffer::RetrieveUntil pointer is outside readable data");
    }
    Retrieve(static_cast<std::size_t>(end - Peek()));
}

void Buffer::RetrieveAll() noexcept {
    reader_index_ = kCheapPrepend;
    writer_index_ = kCheapPrepend;
}

std::string Buffer::RetrieveAsString(std::size_t length) {
    if (length > ReadableBytes()) {
        throw std::out_of_range("Buffer::RetrieveAsString exceeds readable bytes");
    }
    std::string result(Peek(), length);
    Retrieve(length);
    return result;
}

std::string Buffer::RetrieveAllAsString() {
    return RetrieveAsString(ReadableBytes());
}

void Buffer::Append(const void* data, std::size_t length) {
    Append(static_cast<const char*>(data), length);
}

void Buffer::Append(const char* data, std::size_t length) {
    if (length == 0) {
        return;
    }
    if (data == nullptr && length != 0) {
        throw std::invalid_argument("Buffer::Append received null data");
    }
    EnsureWritableBytes(length);
    std::copy(data, data + length, BeginWrite());
    HasWritten(length);
}

void Buffer::Append(const std::string& data) {
    Append(data.data(), data.size());
}

void Buffer::EnsureWritableBytes(std::size_t length) {
    if (WritableBytes() < length) {
        MakeSpace(length);
    }
}

void Buffer::HasWritten(std::size_t length) {
    if (length > WritableBytes()) {
        throw std::out_of_range("Buffer::HasWritten exceeds writable bytes");
    }
    writer_index_ += length;
}

char* Buffer::Begin() noexcept {
    return storage_.data();
}

const char* Buffer::Begin() const noexcept {
    return storage_.data();
}

void Buffer::MakeSpace(std::size_t length) {
    if (WritableBytes() + PrependableBytes() - kCheapPrepend < length) {
        storage_.resize(writer_index_ + length);
        return;
    }

    const std::size_t readable = ReadableBytes();
    std::memmove(Begin() + kCheapPrepend, Peek(), readable);
    reader_index_ = kCheapPrepend;
    writer_index_ = reader_index_ + readable;
}

}  // namespace net
}  // namespace l4lb
