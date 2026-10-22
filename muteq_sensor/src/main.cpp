//
//  main.cpp
//  muteq_sensor
//

#include "circular_buffer.hpp"
#include "config.hpp"
#include "http_egress.hpp"
#include "smoothing.hpp"
#include "spl_db.hpp"
#include "spl_reader.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <print>

using namespace muteq;

static std::atomic<bool> g_shutdown_requested{false};

extern "C" void signal_handler(int) {
    g_shutdown_requested.store(true);
}

circular_buffer<float, 600> buffer_readings;
circular_buffer<float, 600> buffer_events;

void serial_worker(std::stop_token stop, SplMeterReader& reader, AsymmetricEMA& ema, float threshold);
void consumer_worker(std::stop_token stop, SplDatabase& db, HttpEgressClient& http, const EgressConfig& egress_cfg);

int main(int argc, char* argv[]) {
    // Ensure line-buffered stdout so systemd journal captures logs immediately
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // 1. Install signal handlers for graceful shutdown (SIGINT / SIGTERM)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 2. Load unified configuration (client_config.json -> env vars -> CLI flags)
    const AppConfig app_cfg = ConfigLoader::load(argc, argv);

    // 3. Configure HTTP egress client
    EgressConfig egress_cfg;
    egress_cfg.server_url = app_cfg.server_url;
    egress_cfg.server_hmac_secret = app_cfg.server_hmac_secret;
    egress_cfg.device_id = app_cfg.device_id;
    egress_cfg.publish_interval = std::chrono::seconds(app_cfg.publish_interval_seconds);
    egress_cfg.ssl_verify = app_cfg.ssl_verify;

    try {
        SplMeterReader reader(app_cfg.serial_port);
        SplDatabase db(app_cfg.db_path);
        AsymmetricEMA ema(0.5f, 0.05f);
        HttpEgressClient http;

        std::println("=============================================");
        std::println(" MUTEq C++ Sensor Node");
        std::println(" Device Name:               {}", app_cfg.device_name);
        std::println(" Device ID:                 {}", app_cfg.device_id);
        if (!app_cfg.loaded_config_path.empty()) {
            std::println(" Config File:               {}", app_cfg.loaded_config_path);
        }
        std::println(" Connected to SPL meter on: {}", app_cfg.serial_port);
        std::println(" Local SQLite database:     {}", app_cfg.db_path);
        std::println(" Server Egress URL:         {}",
                     egress_cfg.server_url.empty() ? "(none)" : egress_cfg.server_url);
        std::println(" SSL Verification:          {}", app_cfg.ssl_verify ? "Enabled" : "Disabled (Insecure)");
        std::println(" Publish Interval:          {}s", app_cfg.publish_interval_seconds);
        std::println(" Event Threshold level:     {:.1f} dBA", app_cfg.threshold_dba);
        std::println(" Press [Ctrl+C] to cleanly exit.");
        std::println("=============================================");

        // Startup maintenance: purge expired historical records and truncate WAL
        std::println("Performing startup database maintenance...");
        db.prune_old_data(3, 14);
        db.checkpoint();

        std::jthread serial_thread([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                serial_worker(stop, reader, ema, app_cfg.threshold_dba);
            }
        });
        std::jthread db_thread([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                consumer_worker(stop, db, http, egress_cfg);
            }
        });

        // Wait for shutdown signal (SIGINT / SIGTERM)
        while (!g_shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        std::println("\nShutdown requested. Stopping worker threads...");
        serial_thread.request_stop();
        db_thread.request_stop();

    } catch (const std::exception& ex) {
        std::println(std::cerr, "Fatal error: {}", ex.what());
        return 1;
    }

    std::println("Graceful shutdown complete.");
    return 0;
}

void serial_worker(std::stop_token stop, SplMeterReader& reader, AsymmetricEMA& ema, float threshold) {
    // Producer: Serial reader thread

    float window_peak = 0.0f;
    const auto window_start = std::chrono::steady_clock::now();

    // 100ms aggregation window
    while ((std::chrono::steady_clock::now() - window_start) < std::chrono::milliseconds(100) &&
           !stop.stop_requested()) {
        if (auto sample = reader.read_next_sample()) {
            window_peak = std::max(window_peak, sample->db_a);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (window_peak > 0.0f) {
        const float smoothed = ema.update(window_peak);
        buffer_readings.put(smoothed);

        static auto last_heartbeat = std::chrono::steady_clock::now();
        const auto now_steady = std::chrono::steady_clock::now();
        if (now_steady - last_heartbeat >= std::chrono::seconds(30)) {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            std::println("[{}] [Heartbeat] Peak: {:.1f} dBA | Smoothed: {:.1f} dBA", now_ms, window_peak, smoothed);
            last_heartbeat = now_steady;
        }

        if (window_peak >= threshold) {
            std::println(" [THRESHOLD ALERT] Peak: {:.1f} dBA >= {:.1f} dBA", window_peak, threshold);
            buffer_events.put(window_peak);
        }
    }
}

void consumer_worker(std::stop_token stop, SplDatabase& db, HttpEgressClient& http, const EgressConfig& egress_cfg) {
    // Consumer: Database worker and HTTP egress
    bool has_work = false;
    bool has_new_events = false;

    // Drain readings queue into SQLite
    while (auto sample = buffer_readings.get()) {
        has_work = true;
        db.write_reading(*sample, *sample);
    }

    // Drain events queue into SQLite
    while (auto event_peak = buffer_events.get()) {
        has_work = true;
        has_new_events = true;
        db.write_event(*event_peak, *event_peak);
        std::println(" [DB EVENT] Stored threshold event: {:.1f} dBA (Pending unsent: {})", *event_peak,
                     db.count_unsent_events());
    }

    // Periodic maintenance: prune old sent data and checkpoint WAL every 1 hour
    static auto last_prune_time = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_prune_time >= std::chrono::hours(1)) {
        std::println(" [MAINTENANCE] Pruning expired telemetry records...");
        db.prune_old_data(3, 14);
        db.checkpoint();
        last_prune_time = now;
    }

    // Flush to server immediately when high-noise events occur, or periodically when publish_interval elapses
    // Initialize to force an immediate flush upon daemon start
    static auto last_periodic_flush = std::chrono::steady_clock::now() - egress_cfg.publish_interval;
    const bool periodic_due = (now - last_periodic_flush) >= egress_cfg.publish_interval;

    if ((has_new_events || periodic_due) && !egress_cfg.server_url.empty()) {
        auto result = http.flush(db, egress_cfg);
        if (result.readings_sent > 0 || result.events_sent > 0) {
            std::println(" [HTTP EGRESS] Flushed {} readings and {} events to {}", result.readings_sent,
                         result.events_sent, egress_cfg.server_url);
            last_periodic_flush = now;
        } else if (!result.success && !result.error_message.empty()) {
            std::println(std::cerr, " [HTTP EGRESS ERROR] {}", result.error_message);
            // On failure, retry in 30 seconds rather than waiting full interval
            last_periodic_flush = now - egress_cfg.publish_interval + std::chrono::seconds(30);
        } else {
            last_periodic_flush = now;
        }
    }

    if (!has_work && !stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
