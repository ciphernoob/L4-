#include "config/Config.h"

#include <fstream>
#include <json/json.h>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace l4lb {
namespace config {
namespace {

const Json::Value& RequireMember(const Json::Value& object, const char* name,
                                 Json::ValueType type, const std::string& path) {
    if (!object.isObject() || !object.isMember(name)) {
        throw std::invalid_argument(path + "." + name + " is required");
    }
    const Json::Value& value = object[name];
    if (value.type() != type) {
        throw std::invalid_argument(path + "." + name + " has the wrong type");
    }
    return value;
}

std::uint64_t RequireUnsigned(const Json::Value& object, const char* name,
                              std::uint64_t minimum, std::uint64_t maximum,
                              const std::string& path) {
    if (!object.isObject() || !object.isMember(name)) {
        throw std::invalid_argument(path + "." + name + " is required");
    }
    const Json::Value& value = object[name];
    if (!value.isUInt64()) {
        throw std::invalid_argument(path + "." + name + " must be an unsigned integer");
    }
    const Json::UInt64 number = value.asUInt64();
    if (number < minimum || number > maximum) {
        throw std::invalid_argument(path + "." + name + " is out of range");
    }
    return number;
}

net::InetAddress ParseEndpoint(const std::string& value, const std::string& path) {
    const std::size_t separator = value.rfind(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= value.size() || value.find(':') != separator) {
        throw std::invalid_argument(path + " must be an IPv4 address in ip:port form");
    }
    const std::string ip = value.substr(0, separator);
    const std::string port_text = value.substr(separator + 1);
    std::size_t consumed = 0;
    unsigned long port = 0;
    try {
        port = std::stoul(port_text, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(path + " contains an invalid port");
    }
    if (consumed != port_text.size() || port == 0 || port > 65535) {
        throw std::invalid_argument(path + " port must be between 1 and 65535");
    }
    try {
        return net::InetAddress(ip, static_cast<std::uint16_t>(port));
    } catch (const std::exception&) {
        throw std::invalid_argument(path + " contains an invalid IPv4 address");
    }
}

std::chrono::milliseconds Milliseconds(const Json::Value& object, const char* name,
                                       std::uint64_t minimum, std::uint64_t maximum,
                                       const std::string& path) {
    return std::chrono::milliseconds(
        RequireUnsigned(object, name, minimum, maximum, path));
}

}  // namespace

ServerConfig LoadConfigFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open config file: " + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw std::runtime_error("cannot read config file: " + path);
    }
    return ParseConfig(contents.str());
}

ServerConfig ParseConfig(const std::string& json_text) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    Json::Value root;
    std::string errors;
    std::istringstream input(json_text);
    if (!Json::parseFromStream(builder, input, &root, &errors)) {
        throw std::invalid_argument("invalid JSON configuration: " + errors);
    }
    if (!root.isObject()) {
        throw std::invalid_argument("config root must be an object");
    }

    const std::string listen_text =
        RequireMember(root, "listen", Json::stringValue, "config").asString();
    const std::string admin_text =
        RequireMember(root, "admin_listen", Json::stringValue, "config").asString();
    const std::uint64_t workers = RequireUnsigned(root, "workers", 1, 128, "config");
    const std::string algorithm_text =
        RequireMember(root, "algorithm", Json::stringValue, "config").asString();
    Algorithm algorithm;
    if (algorithm_text == "round_robin") {
        algorithm = Algorithm::kRoundRobin;
    } else if (algorithm_text == "least_connections") {
        algorithm = Algorithm::kLeastConnections;
    } else {
        throw std::invalid_argument(
            "config.algorithm must be round_robin or least_connections");
    }

    const Json::Value backend_values =
        RequireMember(root, "backends", Json::arrayValue, "config");
    if (backend_values.empty()) {
        throw std::invalid_argument("config.backends must not be empty");
    }
    std::vector<lb::BackendConfig> backends;
    std::unordered_set<std::string> backend_ids;
    for (Json::ArrayIndex index = 0; index < backend_values.size(); ++index) {
        const Json::Value& item = backend_values[index];
        const std::string path = "config.backends[" + std::to_string(index) + "]";
        const std::string id =
            RequireMember(item, "id", Json::stringValue, path).asString();
        if (id.empty() || !backend_ids.insert(id).second) {
            throw std::invalid_argument(path + ".id must be non-empty and unique");
        }
        const std::string address =
            RequireMember(item, "address", Json::stringValue, path).asString();
        const net::InetAddress endpoint = ParseEndpoint(address, path + ".address");
        const std::uint64_t weight = RequireUnsigned(item, "weight", 1, 1000, path);
        backends.push_back(lb::BackendConfig{id, endpoint.Ip(), endpoint.Port(),
                                             static_cast<int>(weight)});
    }

    const Json::Value timeouts =
        RequireMember(root, "timeouts", Json::objectValue, "config");
    const std::chrono::milliseconds connect_timeout =
        Milliseconds(timeouts, "connect_ms", 1, 300000, "config.timeouts");
    const std::chrono::milliseconds idle_timeout =
        Milliseconds(timeouts, "idle_ms", 1, 86400000, "config.timeouts");

    const Json::Value watermarks =
        RequireMember(root, "buffer_watermarks", Json::objectValue, "config");
    const std::uint64_t low = RequireUnsigned(
        watermarks, "low_bytes", 1, 1024ULL * 1024ULL * 1024ULL,
        "config.buffer_watermarks");
    const std::uint64_t high = RequireUnsigned(
        watermarks, "high_bytes", 2, 1024ULL * 1024ULL * 1024ULL,
        "config.buffer_watermarks");
    if (low >= high) {
        throw std::invalid_argument(
            "config.buffer_watermarks.low_bytes must be less than high_bytes");
    }

    const Json::Value health =
        RequireMember(root, "health_check", Json::objectValue, "config");
    HealthCheckConfig health_check;
    health_check.interval =
        Milliseconds(health, "interval_ms", 10, 3600000, "config.health_check");
    health_check.timeout =
        Milliseconds(health, "timeout_ms", 1, 300000, "config.health_check");
    health_check.failure_threshold = static_cast<unsigned int>(
        RequireUnsigned(health, "failure_threshold", 1, 1000,
                        "config.health_check"));
    health_check.success_threshold = static_cast<unsigned int>(
        RequireUnsigned(health, "success_threshold", 1, 1000,
                        "config.health_check"));
    if (health_check.timeout >= health_check.interval) {
        throw std::invalid_argument(
            "config.health_check.timeout_ms must be less than interval_ms");
    }

    const std::uint64_t retries =
        RequireUnsigned(root, "max_connect_retries", 0, 100, "config");
    const std::chrono::milliseconds grace =
        Milliseconds(root, "shutdown_grace_ms", 0, 3600000, "config");

    return ServerConfig{ParseEndpoint(listen_text, "config.listen"),
                        ParseEndpoint(admin_text, "config.admin_listen"),
                        static_cast<std::size_t>(workers),
                        algorithm,
                        std::move(backends),
                        connect_timeout,
                        idle_timeout,
                        static_cast<std::size_t>(low),
                        static_cast<std::size_t>(high),
                        health_check,
                        static_cast<std::size_t>(retries),
                        grace};
}

const char* AlgorithmName(Algorithm algorithm) noexcept {
    return algorithm == Algorithm::kRoundRobin ? "round_robin" : "least_connections";
}

}  // namespace config
}  // namespace l4lb
