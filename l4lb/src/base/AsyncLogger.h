#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iosfwd>
#include <mutex>
#include <string>
#include <thread>

namespace l4lb {
namespace base {

class AsyncLogger {
public:
    explicit AsyncLogger(std::ostream* output = nullptr);
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void LogSession(std::uint64_t session_id, const std::string& backend_id,
                    const std::string& state, const std::string& close_reason);
    void Flush();
    void Stop();

private:
    void Run();

    std::ostream* output_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable drained_;
    std::deque<std::string> records_;
    bool writing_{false};
    bool stopping_{false};
    std::thread thread_;
};

}  // namespace base
}  // namespace l4lb
