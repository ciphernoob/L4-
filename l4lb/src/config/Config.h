#pragma once

#include "lb/BackendPool.h"
#include "net/InetAddress.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace l4lb {
namespace config {

enum class Algorithm {
    kRoundRobin,
    kLeastConnections,
};

struct HealthCheckConfig {
    std::chrono::milliseconds interval{5000};
    std::chrono::milliseconds timeout{1000};
    unsigned int failure_threshold{3};
    unsigned int success_threshold{2};
};

struct ServerConfig {
    net::InetAddress listen;
    net::InetAddress admin_listen;
    std::size_t workers;
    Algorithm algorithm;
    std::vector<lb::BackendConfig> backends;
    std::chrono::milliseconds connect_timeout;
    std::chrono::milliseconds idle_timeout;
    std::size_t low_watermark;
    std::size_t high_watermark;
    HealthCheckConfig health_check;
    std::size_t max_connect_retries;
    std::chrono::milliseconds shutdown_grace;
};

ServerConfig LoadConfigFile(const std::string& path);
ServerConfig ParseConfig(const std::string& json_text);
const char* AlgorithmName(Algorithm algorithm) noexcept;

}  // namespace config
}  // namespace l4lb
