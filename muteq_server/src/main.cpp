//
//  main.cpp
//  muteq_server
//
//  High-performance C++23 dashboard server replacing FastAPI server.py.
//

#include "auth.hpp"
#include "db.hpp"
#include "httplib.h"
#include "json.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

using json = nlohmann::json;

namespace {

struct ServerConfig {
    std::string host{"0.0.0.0"};
    int port{8080};
    std::string db_path{"/private/tmp/muteq_server.db"};
    std::string hmac_secret{};
    std::string static_dir{"static"};
    json device_info = {
        {"device_name", "MUTEq Sensor"},
        {"location", ""},
        {"environment_profile", ""}
    };
};

ServerConfig load_config() {
    ServerConfig cfg;

    if (const char* p = std::getenv("MUTEQ_PORT")) {
        cfg.port = std::atoi(p);
    }
    if (const char* h = std::getenv("MUTEQ_HOST")) {
        cfg.host = h;
    }
    if (const char* s = std::getenv("MUTEQ_HMAC_SECRET")) {
        cfg.hmac_secret = s;
    }
    if (const char* sd = std::getenv("MUTEQ_STATIC_DIR")) {
        cfg.static_dir = sd;
    } else {
        // Search common static asset locations (e.g. when run from Xcode build dirs)
        const std::string candidate_paths[] = {
            "static",
            "../static",
            "../../static",
            "../../../static"
        };
        for (const auto& candidate : candidate_paths) {
            if (std::filesystem::exists(candidate + "/index.html")) {
                cfg.static_dir = candidate;
                break;
            }
        }
    }

    // Database path resolution
    if (const char* d = std::getenv("MUTEQ_DB")) {
        cfg.db_path = d;
    } else {
        // Check for local client_config.json
        std::ifstream config_file("client_config.json");
        if (config_file.is_open()) {
            try {
                json j = json::parse(config_file);
                if (j.contains("db_path")) {
                    cfg.db_path = j["db_path"];
                }
                if (j.contains("device_name")) {
                    cfg.device_info["device_name"] = j["device_name"];
                }
                if (j.contains("location") && j["location"].is_object()) {
                    cfg.device_info["location"] = j["location"].value("address", "");
                }
                if (j.contains("environment_profile")) {
                    cfg.device_info["environment_profile"] = j["environment_profile"];
                }
            } catch (...) {}
        }
    }

    return cfg;
}

std::optional<int64_t> parse_int_query(const httplib::Request& req, const std::string& key) {
    if (req.has_param(key)) {
        try {
            return std::stoll(req.get_param_value(key));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

int main() {
    ServerConfig cfg = load_config();

    try {
        server::ServerDatabase db(cfg.db_path);
        httplib::Server svr;

        // Configure multi-threaded thread pool
        svr.new_task_queue = [] { return new httplib::ThreadPool(12); };

        // Mount static files directory
        if (::access(cfg.static_dir.c_str(), F_OK) == 0) {
            svr.set_mount_point("/static", cfg.static_dir.c_str());
        }

        // ---------------------------------------------------------------------
        // 1. Root: Serve index.html
        // ---------------------------------------------------------------------
        svr.Get("/", [&cfg](const httplib::Request&, httplib::Response& res) {
            std::string index_path = cfg.static_dir + "/index.html";
            std::ifstream file(index_path);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                res.set_content(buffer.str(), "text/html; charset=utf-8");
            } else {
                res.status = 404;
                res.set_content("<h1>404 Not Found</h1><p>index.html missing</p>", "text/html");
            }
        });

        // ---------------------------------------------------------------------
        // 2. GET /api/config: Return device info
        // ---------------------------------------------------------------------
        svr.Get("/api/config", [&cfg](const httplib::Request&, httplib::Response& res) {
            res.set_content(cfg.device_info.dump(), "application/json");
        });

        // ---------------------------------------------------------------------
        // 3. GET /api/readings: Return time series [{time, value}]
        // ---------------------------------------------------------------------
        svr.Get("/api/readings", [&db](const httplib::Request& req, httplib::Response& res) {
            auto from_ts = parse_int_query(req, "from_ts");
            auto to_ts = parse_int_query(req, "to_ts");
            size_t limit = static_cast<size_t>(parse_int_query(req, "limit").value_or(2000));
            limit = std::clamp(limit, size_t{1}, size_t{10000});

            auto points = db.get_readings(from_ts, to_ts, limit);

            json j = json::array();
            for (const auto& p : points) {
                j.push_back({{"time", p.time}, {"value", std::round(p.value * 10.0f) / 10.0f}});
            }
            res.set_content(j.dump(), "application/json");
        });

        // ---------------------------------------------------------------------
        // 4. GET /api/events: Return threshold events [{time, noise, peak}]
        // ---------------------------------------------------------------------
        svr.Get("/api/events", [&db](const httplib::Request& req, httplib::Response& res) {
            auto from_ts = parse_int_query(req, "from_ts");
            auto to_ts = parse_int_query(req, "to_ts");
            size_t limit = static_cast<size_t>(parse_int_query(req, "limit").value_or(500));
            limit = std::clamp(limit, size_t{1}, size_t{5000});

            auto events = db.get_events(from_ts, to_ts, limit);

            json j = json::array();
            for (const auto& e : events) {
                j.push_back({
                    {"time", e.time},
                    {"noise", std::round(e.noise * 10.0f) / 10.0f},
                    {"peak", std::round(e.peak * 10.0f) / 10.0f}
                });
            }
            res.set_content(j.dump(), "application/json");
        });

        // ---------------------------------------------------------------------
        // 5. GET /api/stats: Return KPI stats, percentiles, histogram, heatmap
        // ---------------------------------------------------------------------
        svr.Get("/api/stats", [&db](const httplib::Request& req, httplib::Response& res) {
            auto from_ts = parse_int_query(req, "from_ts");
            auto stats = db.get_stats(from_ts);

            json j;
            if (stats.latest) {
                j["latest"] = std::round(*stats.latest * 10.0f) / 10.0f;
            } else {
                j["latest"] = nullptr;
            }

            if (stats.peak) {
                j["peak"] = std::round(*stats.peak * 10.0f) / 10.0f;
            } else {
                j["peak"] = nullptr;
            }

            j["event_count"] = stats.event_count;

            j["percentiles"] = {
                {"p50", stats.percentiles.p50 ? json(*stats.percentiles.p50) : json(nullptr)},
                {"p90", stats.percentiles.p90 ? json(*stats.percentiles.p90) : json(nullptr)},
                {"p99", stats.percentiles.p99 ? json(*stats.percentiles.p99) : json(nullptr)}
            };

            j["histogram"] = {
                {"labels", stats.histogram.labels},
                {"counts", stats.histogram.counts}
            };

            j["heatmap"] = stats.heatmap;

            json daily_arr = json::array();
            for (const auto& d : stats.daily_stats) {
                daily_arr.push_back({
                    {"day", d.day},
                    {"avg_noise", d.avg_noise},
                    {"peak_noise", d.peak_noise},
                    {"event_count", d.event_count}
                });
            }
            j["daily_stats"] = daily_arr;

            res.set_content(j.dump(), "application/json");
        });

        // ---------------------------------------------------------------------
        // 6. POST /api/ingest: Receive batch readings & events with HMAC
        // ---------------------------------------------------------------------
        svr.Post("/api/ingest", [&db, &cfg](const httplib::Request& req, httplib::Response& res) {
            // Verify HMAC signature if secret is configured
            if (!cfg.hmac_secret.empty()) {
                std::string sig = req.get_header_value("X-HMAC-Signature");
                if (!server::verify_hmac(cfg.hmac_secret, req.body, sig)) {
                    res.status = 401;
                    res.set_content(json({{"detail", "Invalid signature"}}).dump(), "application/json");
                    return;
                }
            }

            json payload;
            try {
                payload = json::parse(req.body);
            } catch (...) {
                res.status = 400;
                res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
                return;
            }

            std::vector<server::ReadingInput> readings;
            if (payload.contains("readings") && payload["readings"].is_array()) {
                for (const auto& item : payload["readings"]) {
                    readings.push_back({
                        .timestamp = item.value("timestamp", ""),
                        .noise_value = item.value("noise_value", 0.0f),
                        .peak_value = item.value("peak_value", 0.0f)
                    });
                }
            }

            std::vector<server::EventInput> events;
            if (payload.contains("events") && payload["events"].is_array()) {
                for (const auto& item : payload["events"]) {
                    events.push_back({
                        .timestamp = item.value("timestamp", ""),
                        .noise_value = item.value("noise_value", 0.0f),
                        .peak_value = item.value("peak_value", 0.0f)
                    });
                }
            }

            auto [r_cnt, e_cnt] = db.ingest(readings, events);

            json response = {
                {"inserted", {{"readings", r_cnt}, {"events", e_cnt}}}
            };
            res.set_content(response.dump(), "application/json");
        });

        std::println("======================================================");
        std::println(" MUTEq C++23 Dashboard Server");
        std::println(" Listening on:  http://{}:{}", cfg.host, cfg.port);
        std::println(" SQLite DB:     {}", cfg.db_path);
        std::println(" Static Dir:    {}", cfg.static_dir);
        std::println(" HMAC Auth:     {}", cfg.hmac_secret.empty() ? "Disabled (open)" : "Enabled");
        std::println("======================================================");

        if (!svr.listen(cfg.host.c_str(), cfg.port)) {
            std::println(stderr, "Error: Could not bind server to {}:{}", cfg.host, cfg.port);
            return 1;
        }

    } catch (const std::exception& ex) {
        std::println(stderr, "Fatal server error: {}", ex.what());
        return 1;
    }

    return 0;
}
