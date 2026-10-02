//
//  config.hpp
//  muteq_sensor
//
//  Configuration loader supporting client_config.json, environment variables,
//  and command-line arguments matching the Raspberry Pi sensor specification.
//

#pragma once

#include "json.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#    include <CommonCrypto/CommonDigest.h>
#else
#    include <openssl/sha.h>
#endif

namespace muteq {

/// Compute first 16 hex characters of SHA-256 (matching Python sensor client implementation)
inline std::string sha256_prefix_16(std::string_view input) {
#if defined(__APPLE__)
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), md);
#else
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), md);
#endif
    std::string hex;
    hex.reserve(16);
    for (int i = 0; i < 8; ++i) {
        hex += std::format("{:02x}", md[i]);
    }
    return hex;
}

/// Unified application configuration
struct AppConfig {
    std::string device_name{"MUTEq Sensor"};
    std::string device_id;
    std::string db_path{"/tmp/muteq.db"};
    std::string server_url;
    std::string server_hmac_secret;
    std::string serial_port{"/dev/ttyUSB0"};
    int publish_interval_seconds{60};
    float threshold_dba{70.0f};
    bool ssl_verify{true};
    std::string loaded_config_path;
};

class ConfigLoader {
  public:
    static AppConfig load(int argc, char* argv[]) {
        AppConfig cfg;
        std::string explicit_config_path;
        std::string explicit_port;

        // 1. Parse CLI arguments
        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
                explicit_config_path = argv[++i];
            } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
                explicit_port = argv[++i];
            } else if (arg == "--insecure" || arg == "-k" || arg == "--no-ssl-verify") {
                cfg.ssl_verify = false;
            } else if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                std::exit(0);
            } else if (!arg.starts_with("-")) {
                // If it looks like a JSON config file
                if (arg.ends_with(".json")) {
                    explicit_config_path = arg;
                } else {
                    // Positional argument for serial port
                    explicit_port = arg;
                }
            }
        }

        // 2. Discover configuration file path
        std::string config_file;
        if (!explicit_config_path.empty()) {
            config_file = explicit_config_path;
        } else if (const char* env_conf = std::getenv("MUTEQ_CONFIG_PATH")) {
            config_file = env_conf;
        } else if (const char* env_dir = std::getenv("MUTEQ_CONFIG_DIR")) {
            const std::filesystem::path dir(env_dir);
            if (std::filesystem::exists(dir / "client_config.json")) {
                config_file = (dir / "client_config.json").string();
            } else if (std::filesystem::exists(dir / "config_client.json")) {
                config_file = (dir / "config_client.json").string();
            }
        }

        if (config_file.empty()) {
            const std::vector<std::string> candidate_paths = {
                "/home/coder/muteq/sensor/client_config.json",
                "/var/lib/muteq-sensor/client_config.json",
                "/var/lib/muteq-sensor/config_client.json",
                "/etc/muteq/client_config.json",
                "./client_config.json",
            };
            for (const auto& path : candidate_paths) {
                if (std::filesystem::exists(path)) {
                    config_file = path;
                    break;
                }
            }
        }

        // 3. Load from JSON if found
        if (!config_file.empty() && std::filesystem::exists(config_file)) {
            try {
                std::ifstream ifs(config_file);
                if (ifs.is_open()) {
                    nlohmann::json j;
                    ifs >> j;

                    if (j.contains("device_name") && j["device_name"].is_string()) {
                        cfg.device_name = j["device_name"].get<std::string>();
                    }
                    if (j.contains("local_device_id") && j["local_device_id"].is_string() &&
                        !j["local_device_id"].get<std::string>().empty()) {
                        cfg.device_id = j["local_device_id"].get<std::string>();
                    }
                    if (j.contains("db_path") && j["db_path"].is_string()) {
                        cfg.db_path = j["db_path"].get<std::string>();
                    }
                    if (j.contains("server_url") && j["server_url"].is_string()) {
                        cfg.server_url = j["server_url"].get<std::string>();
                    }
                    if (j.contains("server_hmac_secret") && j["server_hmac_secret"].is_string()) {
                        cfg.server_hmac_secret = j["server_hmac_secret"].get<std::string>();
                    }
                    if (j.contains("publish_interval_seconds") && j["publish_interval_seconds"].is_number()) {
                        cfg.publish_interval_seconds = j["publish_interval_seconds"].get<int>();
                    }
                    if (j.contains("ssl_verify") && j["ssl_verify"].is_boolean()) {
                        cfg.ssl_verify = j["ssl_verify"].get<bool>();
                    }
                    if (j.contains("insecure_ssl") && j["insecure_ssl"].is_boolean()) {
                        cfg.ssl_verify = !j["insecure_ssl"].get<bool>();
                    }

                    cfg.loaded_config_path = config_file;
                }
            } catch (const std::exception& e) {
                std::println(std::cerr, "Warning: Failed to parse config file '{}': {}", config_file, e.what());
            }
        }

        // 4. Fallback: derive device_id from device_name if not provided
        if (cfg.device_id.empty()) {
            cfg.device_id = sha256_prefix_16(cfg.device_name);
        }

        // 5. Apply Environment Variable Overrides
        if (const char* v = std::getenv("MUTEQ_DEVICE_NAME")) {
            cfg.device_name = v;
        }
        if (const char* v = std::getenv("MUTEQ_DEVICE_ID")) {
            cfg.device_id = v;
        }
        if (const char* v = std::getenv("MUTEQ_SERVER_URL")) {
            cfg.server_url = v;
        }
        if (const char* v = std::getenv("MUTEQ_HMAC_SECRET")) {
            cfg.server_hmac_secret = v;
        }
        if (const char* v = std::getenv("MUTEQ_DB_PATH")) {
            cfg.db_path = v;
        }
        if (const char* v = std::getenv("MUTEQ_SERIAL_PORT")) {
            cfg.serial_port = v;
        }
        if (const char* v = std::getenv("MUTEQ_PUBLISH_INTERVAL")) {
            try {
                cfg.publish_interval_seconds = std::stoi(v);
            } catch (...) {
            }
        }
        if (const char* v = std::getenv("MUTEQ_SSL_VERIFY")) {
            std::string_view s(v);
            if (s == "0" || s == "false" || s == "FALSE" || s == "no" || s == "NO") {
                cfg.ssl_verify = false;
            } else if (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "YES") {
                cfg.ssl_verify = true;
            }
        }
        if (const char* v = std::getenv("MUTEQ_INSECURE_SSL")) {
            std::string_view s(v);
            if (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "YES") {
                cfg.ssl_verify = false;
            }
        }

        // 6. Explicit CLI port argument takes highest precedence
        if (!explicit_port.empty()) {
            cfg.serial_port = explicit_port;
        } else if (cfg.serial_port == "/dev/ttyUSB0" && !std::filesystem::exists("/dev/ttyUSB0") &&
                   std::filesystem::exists("/tmp/ttyUSB0")) {
            // Auto-detect mock serial port if real hardware device not present
            cfg.serial_port = "/tmp/ttyUSB0";
        }

        // 7. Ensure DB parent directory exists or fallback safely
        std::filesystem::path p(cfg.db_path);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
            if (ec) {
                std::println(std::cerr,
                             "Warning: Cannot create directory '{}': {}. Falling back db_path to /tmp/muteq.db",
                             p.parent_path().string(), ec.message());
                cfg.db_path = "/tmp/muteq.db";
            }
        }

        return cfg;
    }

  private:
    static void print_usage(const char* prog) {
        std::println("Usage: {} [options] [serial_port]", prog);
        std::println("Options:");
        std::println("  -c, --config <path>   Path to client_config.json");
        std::println("  -p, --port <port>     Serial port (e.g. /dev/ttyUSB0 or /tmp/ttyUSB0)");
        std::println("  -k, --insecure        Allow insecure server connection (disable SSL certificate check)");
        std::println("  -h, --help            Show this help message and exit");
        std::println("");
        std::println("Environment Variables:");
        std::println("  MUTEQ_CONFIG_PATH     Path to client_config.json");
        std::println("  MUTEQ_CONFIG_DIR      Directory containing client_config.json");
        std::println("  MUTEQ_SERIAL_PORT     Serial port path");
        std::println("  MUTEQ_SERVER_URL      Dashboard/Ingest server URL");
        std::println("  MUTEQ_HMAC_SECRET     HMAC-SHA256 signature secret");
        std::println("  MUTEQ_DEVICE_ID       Local device identifier");
        std::println("  MUTEQ_DB_PATH         Local SQLite database file path");
        std::println("  MUTEQ_PUBLISH_INTERVAL Periodic HTTP flush interval in seconds");
        std::println("  MUTEQ_SSL_VERIFY      Set to 0 to disable SSL certificate verification");
    }
};

} // namespace muteq
