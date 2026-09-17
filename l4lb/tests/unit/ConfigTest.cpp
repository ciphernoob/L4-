#include "Test.h"

#include "config/Config.h"

#include <stdexcept>
#include <string>

using l4lb::config::Algorithm;
using l4lb::config::ParseConfig;
using l4lb::config::ServerConfig;

namespace {

const char* ValidConfig() {
    return R"json({
        "listen":"127.0.0.1:9000",
        "admin_listen":"127.0.0.1:9001",
        "workers":2,
        "algorithm":"round_robin",
        "backends":[{"id":"one","address":"127.0.0.1:9101","weight":1}],
        "timeouts":{"connect_ms":500,"idle_ms":30000},
        "buffer_watermarks":{"low_bytes":4096,"high_bytes":16384},
        "health_check":{"interval_ms":5000,"timeout_ms":500,
                          "failure_threshold":3,"success_threshold":2},
        "max_connect_retries":1,
        "shutdown_grace_ms":5000
    })json";
}

bool Rejects(const std::string& text, const std::string& field) {
    try {
        (void)ParseConfig(text);
    } catch (const std::invalid_argument& error) {
        return std::string(error.what()).find(field) != std::string::npos;
    }
    return false;
}

}  // namespace

L4LB_TEST(ConfigParsesAllRuntimeFields) {
    const ServerConfig config = ParseConfig(ValidConfig());
    L4LB_REQUIRE(config.listen.ToString() == "127.0.0.1:9000");
    L4LB_REQUIRE(config.admin_listen.ToString() == "127.0.0.1:9001");
    L4LB_REQUIRE(config.workers == 2);
    L4LB_REQUIRE(config.algorithm == Algorithm::kRoundRobin);
    L4LB_REQUIRE(config.backends.size() == 1);
    L4LB_REQUIRE(config.connect_timeout.count() == 500);
    L4LB_REQUIRE(config.idle_timeout.count() == 30000);
    L4LB_REQUIRE(config.low_watermark == 4096);
    L4LB_REQUIRE(config.high_watermark == 16384);
    L4LB_REQUIRE(config.max_connect_retries == 1);
    L4LB_REQUIRE(config.shutdown_grace.count() == 5000);
}

L4LB_TEST(ConfigRejectsInvalidAlgorithmWatermarksAndBackends) {
    std::string invalid_algorithm = ValidConfig();
    invalid_algorithm.replace(invalid_algorithm.find("round_robin"), 11, "random");
    L4LB_REQUIRE(Rejects(invalid_algorithm, "algorithm"));

    std::string invalid_watermark = ValidConfig();
    invalid_watermark.replace(invalid_watermark.find("\"low_bytes\":4096"), 16,
                              "\"low_bytes\":20000");
    L4LB_REQUIRE(Rejects(invalid_watermark, "low_bytes"));

    std::string no_backends = ValidConfig();
    const std::size_t begin = no_backends.find("[{\"id\"");
    const std::size_t end = no_backends.find("}]", begin);
    no_backends.replace(begin, end - begin + 2, "[]");
    L4LB_REQUIRE(Rejects(no_backends, "backends"));
}

L4LB_TEST(ConfigRejectsInvalidEndpointAndTimeoutRelation) {
    std::string invalid_endpoint = ValidConfig();
    invalid_endpoint.replace(invalid_endpoint.find("127.0.0.1:9000"), 14,
                             "not-an-address");
    L4LB_REQUIRE(Rejects(invalid_endpoint, "listen"));

    std::string invalid_health = ValidConfig();
    invalid_health.replace(invalid_health.find("\"timeout_ms\":500"), 16,
                           "\"timeout_ms\":6000");
    L4LB_REQUIRE(Rejects(invalid_health, "timeout_ms"));
}
