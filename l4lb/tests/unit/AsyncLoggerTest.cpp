#include "Test.h"

#include "base/AsyncLogger.h"

#include <sstream>
#include <string>
#include <thread>
#include <vector>

L4LB_TEST(AsyncLoggerSerializesConcurrentRecordsAndFlushesOnStop) {
    std::ostringstream output;
    l4lb::base::AsyncLogger logger(&output);
    std::vector<std::thread> writers;
    for (std::uint64_t worker = 0; worker < 4; ++worker) {
        writers.emplace_back([worker, &logger] {
            for (std::uint64_t index = 0; index < 100; ++index) {
                logger.LogSession(worker * 100 + index, "backend-\"one",
                                  "established", "none");
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }
    logger.Stop();

    const std::string records = output.str();
    std::size_t lines = 0;
    for (const char character : records) {
        if (character == '\n') {
            ++lines;
        }
    }
    L4LB_REQUIRE(lines == 400);
    L4LB_REQUIRE(records.find("\"backend_id\":\"backend-\\\"one\"") !=
                  std::string::npos);
    L4LB_REQUIRE(records.find("\"state\":\"established\"") !=
                  std::string::npos);
}
