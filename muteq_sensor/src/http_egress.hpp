//
//  http_egress.hpp
//  muteq_sensor
//
//  HTTP/HTTPS egress worker using libcurl and HMAC-SHA256 authentication.
//  Drains buffered readings and events from SQLite and POSTs them to the dashboard server.
//

#pragma once

#include "spl_db.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <curl/curl.h>
#include <format>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#    include <CommonCrypto/CommonHMAC.h>
#else
#    include <openssl/evp.h>
#    include <openssl/hmac.h>
#endif

namespace muteq {

/// Compute HMAC-SHA256 hex digest for request signing
inline std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
#if defined(__APPLE__)
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), data.data(), data.size(), md);
    std::string out;
    out.reserve(CC_SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        out += std::format("{:02x}", md[i]);
    }
    return out;
#else
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(data.data()),
         data.size(), md, &md_len);
    std::string out;
    out.reserve(md_len * 2);
    for (unsigned int i = 0; i < md_len; ++i) {
        out += std::format("{:02x}", md[i]);
    }
    return out;
#endif
}

/// Serialize telemetry batch into JSON payload accepted by /api/ingest
inline std::string serialize_ingest_payload(std::string_view device_id, std::span<const TelemetryRecord> readings,
                                            std::span<const TelemetryRecord> events) {
    std::string out;
    out.reserve(128 + (readings.size() + events.size()) * 95);

    out.append("{\"device_id\":\"");
    out.append(device_id);
    out.append("\",\"readings\":[");

    for (size_t i = 0; i < readings.size(); ++i) {
        if (i > 0)
            out.push_back(',');
        out.append("{\"timestamp\":\"");
        out.append(readings[i].timestamp);
        out.append(std::format("\",\"noise_value\":{:.1f},\"peak_value\":{:.1f}}}", readings[i].noise_value,
                               readings[i].peak_value));
    }

    out.append("],\"events\":[");

    for (size_t i = 0; i < events.size(); ++i) {
        if (i > 0)
            out.push_back(',');
        out.append("{\"timestamp\":\"");
        out.append(events[i].timestamp);
        out.append(std::format("\",\"noise_value\":{:.1f},\"peak_value\":{:.1f}}}", events[i].noise_value,
                               events[i].peak_value));
    }

    out.append("]}");
    return out;
}

/// Configuration settings for the HTTP egress client
struct EgressConfig {
    std::string server_url;         /// Base server URL, e.g. "http://127.0.0.1:8080"
    std::string server_hmac_secret; /// Secret key matching MUTEQ_HMAC_SECRET
    std::string device_id;          /// Unique device identifier
    bool ssl_verify{true};          /// Verify SSL peer certificates
    std::chrono::seconds timeout{10};
    size_t batch_readings_limit{5000};
    size_t batch_events_limit{2000};
    size_t max_catchup_loops{10};
    std::chrono::seconds publish_interval{60};
};

/// Result summary of an egress flush operation
struct FlushResult {
    size_t readings_sent{0};
    size_t events_sent{0};
    bool success{false};
    std::string error_message;
};

/// Synchronous HTTP egress client wrapping libcurl
class HttpEgressClient {
  public:
    HttpEgressClient() {
        curl_ = ::curl_easy_init();
    }

    ~HttpEgressClient() {
        if (curl_) {
            ::curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
    }

    HttpEgressClient(const HttpEgressClient&) = delete;
    HttpEgressClient& operator=(const HttpEgressClient&) = delete;

    HttpEgressClient(HttpEgressClient&& other) noexcept : curl_(std::exchange(other.curl_, nullptr)) {
    }

    HttpEgressClient& operator=(HttpEgressClient&& other) noexcept {
        if (this != &other) {
            if (curl_)
                ::curl_easy_cleanup(curl_);
            curl_ = std::exchange(other.curl_, nullptr);
        }
        return *this;
    }

    /// Read unsent rows from SQLite and POST them to server_url/api/ingest
    FlushResult flush(SqliteStorage& storage, const EgressConfig& config) {
        FlushResult total_result{.readings_sent = 0, .events_sent = 0, .success = true, .error_message = ""};

        if (config.server_url.empty()) {
            total_result.success = false;
            total_result.error_message = "Server URL is empty";
            return total_result;
        }

        if (!curl_) {
            total_result.success = false;
            total_result.error_message = "CURL handle not initialized";
            return total_result;
        }

        std::string ingest_url = config.server_url;
        if (ingest_url.ends_with('/')) {
            ingest_url += "api/ingest";
        } else {
            ingest_url += "/api/ingest";
        }

        // Loop up to max_catchup_loops to catch up on accumulated backlog
        for (size_t loop = 0; loop < config.max_catchup_loops; ++loop) {
            auto readings = storage.get_unsent_readings(config.batch_readings_limit);
            auto events = storage.get_unsent_events(config.batch_events_limit);

            if (readings.empty() && events.empty()) {
                break; // Fully caught up
            }

            const std::string payload = serialize_ingest_payload(config.device_id, readings, events);
            curl_slist* headers = nullptr;
            headers = ::curl_slist_append(headers, "Content-Type: application/json");

            if (!config.server_hmac_secret.empty()) {
                const std::string signature = hmac_sha256_hex(config.server_hmac_secret, payload);
                const std::string hmac_header = std::format("X-HMAC-Signature: sha256={}", signature);
                headers = ::curl_slist_append(headers, hmac_header.c_str());
            }

            std::string response_body;
            ::curl_easy_setopt(curl_, CURLOPT_URL, ingest_url.c_str());
            ::curl_easy_setopt(curl_, CURLOPT_POST, 1L);
            ::curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, payload.c_str());
            ::curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
            ::curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
            ::curl_easy_setopt(curl_, CURLOPT_TIMEOUT, static_cast<long>(config.timeout.count()));
            ::curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, config.ssl_verify ? 1L : 0L);
            ::curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, config.ssl_verify ? 2L : 0L);

            CURLcode res = ::curl_easy_perform(curl_);
            ::curl_slist_free_all(headers);

            if (res != CURLE_OK) {
                total_result.success = false;
                if (res == CURLE_PEER_FAILED_VERIFICATION) {
                    total_result.error_message =
                        std::format("SSL certificate verification failed (expired or invalid certificate). Use -k / "
                                    "--insecure or set \"ssl_verify\": false in config: {}",
                                    ::curl_easy_strerror(res));
                } else {
                    total_result.error_message = ::curl_easy_strerror(res);
                }
                break;
            }

            long http_code = 0;
            ::curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code == 200) {
                // Success: mark local rows sent
                std::vector<int64_t> reading_ids;
                reading_ids.reserve(readings.size());
                for (const auto& r : readings)
                    reading_ids.push_back(r.id);

                std::vector<int64_t> event_ids;
                event_ids.reserve(events.size());
                for (const auto& e : events)
                    event_ids.push_back(e.id);

                storage.mark_readings_sent(reading_ids);
                storage.mark_events_sent(event_ids);

                total_result.readings_sent += readings.size();
                total_result.events_sent += events.size();
            } else {
                total_result.success = false;
                total_result.error_message = std::format("Server returned HTTP {}", http_code);
                break; // Stop and retry next interval on error
            }
        }

        return total_result;
    }

    /// Flush unsent rows from SplDatabase
    FlushResult flush(SplDatabase& db, const EgressConfig& config) {
        return flush(db.raw_storage(), config);
    }

  private:
    CURL* curl_{nullptr};
};

/// Dedicated background egress worker running periodic flushes
class HttpEgressWorker {
  public:
    HttpEgressWorker(SqliteStorage& storage, EgressConfig config) : storage_(storage), config_(std::move(config)) {
        ::curl_global_init(CURL_GLOBAL_DEFAULT);
        worker_thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    HttpEgressWorker(SplDatabase& db, EgressConfig config) : HttpEgressWorker(db.raw_storage(), std::move(config)) {
    }

    ~HttpEgressWorker() {
        stop();
        ::curl_global_cleanup();
    }

    /// Wake up the worker thread immediately to perform a flush
    void trigger_flush() {
        {
            std::lock_guard<std::mutex> lock(cv_mutex_);
            trigger_requested_ = true;
        }
        cv_.notify_one();
    }

    /// Stop the background worker
    void stop() {
        worker_thread_.request_stop();
        cv_.notify_all();
    }

  private:
    void run(std::stop_token stop) {
        HttpEgressClient client;

        while (!stop.stop_requested()) {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, config_.publish_interval, [&]() { return stop.stop_requested() || trigger_requested_; });

            if (stop.stop_requested()) {
                break;
            }

            trigger_requested_ = false;
            lock.unlock();

            // Perform flush
            auto res = client.flush(storage_, config_);
            if (!res.success && !res.error_message.empty()) {
                std::cerr << "[SERVER] Flush error: " << res.error_message << "\n";
            } else if (res.readings_sent > 0 || res.events_sent > 0) {
                std::cout << std::format("[SERVER] Flushed {} readings and {} events to {}\n", res.readings_sent,
                                         res.events_sent, config_.server_url);
            }
        }
    }

    SqliteStorage& storage_;
    EgressConfig config_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
    bool trigger_requested_{false};
    std::jthread worker_thread_;
};

} // namespace muteq
