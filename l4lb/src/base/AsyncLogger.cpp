#include "base/AsyncLogger.h"

#include <iostream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace l4lb {
namespace base {
namespace {

std::string EscapeJson(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += character;
            break;
        }
    }
    return result;
}

}  // namespace

AsyncLogger::AsyncLogger(std::ostream* output)
    : output_(output == nullptr ? &std::clog : output),
      thread_([this] { Run(); }) {}

AsyncLogger::~AsyncLogger() {
    Stop();
}

void AsyncLogger::LogSession(std::uint64_t session_id,
                             const std::string& backend_id,
                             const std::string& state,
                             const std::string& close_reason) {
    std::ostringstream record;
    record << "{\"session_id\":" << session_id << ",\"backend_id\":\""
           << EscapeJson(backend_id) << "\",\"state\":\""
           << EscapeJson(state) << "\",\"close_reason\":\""
           << EscapeJson(close_reason) << "\"}\n";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        records_.push_back(record.str());
    }
    ready_.notify_one();
}

void AsyncLogger::Flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    drained_.wait(lock, [this] { return records_.empty() && !writing_; });
    output_->flush();
}

void AsyncLogger::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            if (thread_.joinable()) {
                // Another caller cannot join: Stop is only used by the owner thread.
            } else {
                return;
            }
        }
        stopping_ = true;
    }
    ready_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void AsyncLogger::Run() {
    while (true) {
        std::string record;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !records_.empty(); });
            if (records_.empty() && stopping_) {
                drained_.notify_all();
                return;
            }
            record = std::move(records_.front());
            records_.pop_front();
            writing_ = true;
        }
        *output_ << record;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            writing_ = false;
            if (records_.empty()) {
                output_->flush();
                drained_.notify_all();
            }
        }
    }
}

}  // namespace base
}  // namespace l4lb
