//
//  spl_db.hpp
//  muteq_sensor
//
//  High-level thread-safe SQLite database interface managing local storage of SPL values.
//

#pragma once

#include "spl_reader.hpp"
#include "sqlite_storage.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace muteq {

/// Convert a chrono time point to UTC ISO-8601 string: YYYY-MM-DDTHH:MM:SS.sssZ
inline std::string to_iso8601(std::chrono::system_clock::time_point tp = std::chrono::system_clock::now()) {
    const auto total_millis = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    const auto millis = static_cast<int>(total_millis % 1000);
    const auto seconds = static_cast<std::time_t>(total_millis / 1000);

    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &seconds);
#else
    gmtime_r(&seconds, &tm_buf);
#endif

    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}Z", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                       tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, millis);
}

/// Convenience overload for current timestamp in UTC ISO-8601 format
inline std::string current_iso8601() {
    return to_iso8601();
}

/// Sound level reading record
struct ReadingRecord {
    int64_t id{0};
    std::string timestamp;   // ISO 8601 UTC
    float noise_value{0.0f}; // Smoothed noise level
    float peak_value{0.0f};  // Raw peak level
    bool sent{false};
};

/// Sound level threshold-breach event record
struct EventRecord {
    int64_t id{0};
    std::string timestamp; // ISO 8601 UTC
    float noise_value{0.0f};
    float peak_value{0.0f};
    bool sent{false};
};

/// Count of events in an hour-of-day bucket (0-23)
struct HourlyEventCount {
    int hour{0};
    int64_t count{0};
};

/// Aggregated daily statistics
struct DailyStat {
    std::string day; // YYYY-MM-DD
    double avg_noise{0.0};
    double peak_noise{0.0};
    int64_t event_count{0};
};

/// Thread-safe domain manager for sound pressure readings and threshold events
class SplDatabase {
  public:
    explicit SplDatabase(const std::string_view& db_path) : storage_(db_path) {
        init_statements();
    }

    ~SplDatabase() = default;

    // Non-copyable, movable
    SplDatabase(const SplDatabase&) = delete;
    SplDatabase& operator=(const SplDatabase&) = delete;

    SplDatabase(SplDatabase&& other) noexcept
        : storage_(std::move(other.storage_)), insert_reading_stmt_(std::move(other.insert_reading_stmt_)),
          insert_event_stmt_(std::move(other.insert_event_stmt_)),
          unsent_readings_stmt_(std::move(other.unsent_readings_stmt_)),
          unsent_events_stmt_(std::move(other.unsent_events_stmt_)),
          count_unsent_readings_stmt_(std::move(other.count_unsent_readings_stmt_)),
          count_unsent_events_stmt_(std::move(other.count_unsent_events_stmt_)) {
    }

    SplDatabase& operator=(SplDatabase&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(mutex_, other.mutex_);
            storage_ = std::move(other.storage_);
            insert_reading_stmt_ = std::move(other.insert_reading_stmt_);
            insert_event_stmt_ = std::move(other.insert_event_stmt_);
            unsent_readings_stmt_ = std::move(other.unsent_readings_stmt_);
            unsent_events_stmt_ = std::move(other.unsent_events_stmt_);
            count_unsent_readings_stmt_ = std::move(other.count_unsent_readings_stmt_);
            count_unsent_events_stmt_ = std::move(other.count_unsent_events_stmt_);
        }
        return *this;
    }

    // =========================================================================
    // Insertion API
    // =========================================================================

    /// Record a single reading with explicit timestamp
    void write_reading(std::string_view timestamp_iso, float noise_value, float peak_value) {
        std::lock_guard<std::mutex> lock(mutex_);
        insert_reading_stmt_.reset();
        insert_reading_stmt_.bind_text(1, timestamp_iso);
        insert_reading_stmt_.bind_double(2, static_cast<double>(noise_value));
        insert_reading_stmt_.bind_double(3, static_cast<double>(peak_value));
        insert_reading_stmt_.execute_done();
    }

    /// Record a single reading using current UTC time
    void write_reading(float noise_value, float peak_value) {
        write_reading(current_iso8601(), noise_value, peak_value);
    }

    /// Record a threshold breach event with explicit timestamp
    void write_event(std::string_view timestamp_iso, float noise_value, float peak_value) {
        std::lock_guard<std::mutex> lock(mutex_);
        insert_event_stmt_.reset();
        insert_event_stmt_.bind_text(1, timestamp_iso);
        insert_event_stmt_.bind_double(2, static_cast<double>(noise_value));
        insert_event_stmt_.bind_double(3, static_cast<double>(peak_value));
        insert_event_stmt_.execute_done();
    }

    /// Record a threshold breach event using current UTC time
    void write_event(float noise_value, float peak_value) {
        write_event(current_iso8601(), noise_value, peak_value);
    }

    /// Primary sensor ingestion pipeline helper:
    /// Records reading into readings table and automatically records an event if raw peak >= threshold_db.
    void record_measurement(const SplReading& sample, float smoothed_db, float threshold_db = 70.0f) {
        const std::string ts = current_iso8601();
        record_measurement(ts, sample.db_a, smoothed_db, threshold_db);
    }

    /// Pipeline helper with explicit timestamp
    void record_measurement(std::string_view timestamp_iso, float raw_peak, float smoothed_db,
                            float threshold_db = 70.0f) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto tx = storage_.begin_transaction();

        insert_reading_stmt_.reset();
        insert_reading_stmt_.bind_text(1, timestamp_iso);
        insert_reading_stmt_.bind_double(2, static_cast<double>(smoothed_db));
        insert_reading_stmt_.bind_double(3, static_cast<double>(raw_peak));
        insert_reading_stmt_.execute_done();

        if (raw_peak >= threshold_db) {
            insert_event_stmt_.reset();
            insert_event_stmt_.bind_text(1, timestamp_iso);
            insert_event_stmt_.bind_double(2, static_cast<double>(smoothed_db));
            insert_event_stmt_.bind_double(3, static_cast<double>(raw_peak));
            insert_event_stmt_.execute_done();
        }

        tx.commit();
    }

    // =========================================================================
    // Query API (Matching mute/sensor/app/db.py & dashboard queries)
    // =========================================================================

    /// Return readings since `since_iso`, oldest first, capped at `limit`
    [[nodiscard]] std::vector<ReadingRecord> query_readings(std::string_view since_iso, size_t limit = 2000) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ReadingRecord> results;
        results.reserve(std::min(limit, size_t{2000}));

        auto stmt = storage_.prepare("SELECT timestamp, noise_value, peak_value FROM readings "
                                     "WHERE timestamp >= ? ORDER BY timestamp ASC LIMIT ?;");
        stmt.bind_text(1, since_iso);
        stmt.bind_int64(2, static_cast<int64_t>(limit));

        while (stmt.step() == SQLITE_ROW) {
            results.push_back(ReadingRecord{.id = 0,
                                            .timestamp = std::string(stmt.column_text(0)),
                                            .noise_value = static_cast<float>(stmt.column_double(1)),
                                            .peak_value = static_cast<float>(stmt.column_double(2)),
                                            .sent = false});
        }
        return results;
    }

    /// Return events since `since_iso`, newest first, capped at `limit`
    [[nodiscard]] std::vector<EventRecord> query_events(std::string_view since_iso, size_t limit = 2000) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<EventRecord> results;
        results.reserve(std::min(limit, size_t{2000}));

        auto stmt = storage_.prepare("SELECT timestamp, noise_value, peak_value FROM events "
                                     "WHERE timestamp >= ? ORDER BY timestamp DESC LIMIT ?;");
        stmt.bind_text(1, since_iso);
        stmt.bind_int64(2, static_cast<int64_t>(limit));

        while (stmt.step() == SQLITE_ROW) {
            results.push_back(EventRecord{.id = 0,
                                          .timestamp = std::string(stmt.column_text(0)),
                                          .noise_value = static_cast<float>(stmt.column_double(1)),
                                          .peak_value = static_cast<float>(stmt.column_double(2)),
                                          .sent = false});
        }
        return results;
    }

    /// Return event counts grouped by hour-of-day (0-23) since `since_iso`
    [[nodiscard]] std::vector<HourlyEventCount> query_hourly_event_counts(std::string_view since_iso) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<HourlyEventCount> counts;

        auto stmt = storage_.prepare(
            "SELECT CAST(strftime('%H', timestamp, 'localtime') AS INTEGER) AS hour, COUNT(*) AS count "
            "FROM events WHERE timestamp >= ? GROUP BY hour ORDER BY hour;");
        stmt.bind_text(1, since_iso);

        while (stmt.step() == SQLITE_ROW) {
            counts.push_back(HourlyEventCount{.hour = stmt.column_int(0), .count = stmt.column_int64(1)});
        }
        return counts;
    }

    /// Return per-day average/peak from readings and event counts (newest first, up to 30 days)
    [[nodiscard]] std::vector<DailyStat> query_daily_stats(std::string_view since_iso) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<DailyStat> stats;

        auto stmt_readings =
            storage_.prepare("SELECT date(timestamp, 'localtime') AS day, "
                             "  ROUND(AVG(noise_value), 1) AS avg_noise, "
                             "  ROUND(MAX(noise_value), 1) AS peak_noise "
                             "FROM readings WHERE timestamp >= ? GROUP BY day ORDER BY day DESC LIMIT 30;");
        stmt_readings.bind_text(1, since_iso);

        while (stmt_readings.step() == SQLITE_ROW) {
            stats.push_back(DailyStat{.day = std::string(stmt_readings.column_text(0)),
                                      .avg_noise = stmt_readings.column_double(1),
                                      .peak_noise = stmt_readings.column_double(2),
                                      .event_count = 0});
        }

        // Fetch event count per day
        auto stmt_events = storage_.prepare("SELECT date(timestamp, 'localtime') AS day, COUNT(*) AS event_count "
                                            "FROM events WHERE timestamp >= ? GROUP BY day;");
        stmt_events.bind_text(1, since_iso);

        while (stmt_events.step() == SQLITE_ROW) {
            std::string day = std::string(stmt_events.column_text(0));
            int64_t count = stmt_events.column_int64(1);
            for (auto& s : stats) {
                if (s.day == day) {
                    s.event_count = count;
                    break;
                }
            }
        }

        return stats;
    }

    // =========================================================================
    // Egress & Syncing API
    // =========================================================================

    /// Get unsent readings for remote HTTP egress
    [[nodiscard]] std::vector<ReadingRecord> get_unsent_readings(size_t limit = 5000) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ReadingRecord> records;
        records.reserve(std::min(limit, size_t{5000}));

        unsent_readings_stmt_.reset();
        unsent_readings_stmt_.bind_int64(1, static_cast<int64_t>(limit));

        while (unsent_readings_stmt_.step() == SQLITE_ROW) {
            records.push_back(ReadingRecord{.id = unsent_readings_stmt_.column_int64(0),
                                            .timestamp = std::string(unsent_readings_stmt_.column_text(1)),
                                            .noise_value = static_cast<float>(unsent_readings_stmt_.column_double(2)),
                                            .peak_value = static_cast<float>(unsent_readings_stmt_.column_double(3)),
                                            .sent = false});
        }
        return records;
    }

    /// Get unsent threshold events for remote HTTP egress
    [[nodiscard]] std::vector<EventRecord> get_unsent_events(size_t limit = 2000) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<EventRecord> records;
        records.reserve(std::min(limit, size_t{2000}));

        unsent_events_stmt_.reset();
        unsent_events_stmt_.bind_int64(1, static_cast<int64_t>(limit));

        while (unsent_events_stmt_.step() == SQLITE_ROW) {
            records.push_back(EventRecord{.id = unsent_events_stmt_.column_int64(0),
                                          .timestamp = std::string(unsent_events_stmt_.column_text(1)),
                                          .noise_value = static_cast<float>(unsent_events_stmt_.column_double(2)),
                                          .peak_value = static_cast<float>(unsent_events_stmt_.column_double(3)),
                                          .sent = false});
        }
        return records;
    }

    /// Mark readings as successfully sent to remote server
    void mark_readings_sent(std::span<const int64_t> ids) {
        if (ids.empty())
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        storage_.mark_readings_sent(ids);
    }

    /// Mark events as successfully sent to remote server
    void mark_events_sent(std::span<const int64_t> ids) {
        if (ids.empty())
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        storage_.mark_events_sent(ids);
    }

    /// Count pending unsent readings
    [[nodiscard]] size_t count_unsent_readings() {
        std::lock_guard<std::mutex> lock(mutex_);
        count_unsent_readings_stmt_.reset();
        if (count_unsent_readings_stmt_.step() == SQLITE_ROW) {
            return static_cast<size_t>(count_unsent_readings_stmt_.column_int64(0));
        }
        return 0;
    }

    /// Count pending unsent events
    [[nodiscard]] size_t count_unsent_events() {
        std::lock_guard<std::mutex> lock(mutex_);
        count_unsent_events_stmt_.reset();
        if (count_unsent_events_stmt_.step() == SQLITE_ROW) {
            return static_cast<size_t>(count_unsent_events_stmt_.column_int64(0));
        }
        return 0;
    }

    // =========================================================================
    // Maintenance & Flash Wear Protection
    // =========================================================================

    /// Delete sent readings/events older than retain_days, and unsent older than unsent_retain_days
    void prune_old_data(int retain_days = 3, int unsent_retain_days = 14) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::system_clock::now();
        const std::string cutoff_sent = to_iso8601(now - std::chrono::days(retain_days));
        const std::string cutoff_unsent = to_iso8601(now - std::chrono::days(unsent_retain_days));

        storage_.prune_old_data(cutoff_sent, cutoff_unsent);
    }

    /// Access underlying SqliteStorage if direct low-level API is required
    [[nodiscard]] SqliteStorage& raw_storage() noexcept {
        return storage_;
    }
    [[nodiscard]] const SqliteStorage& raw_storage() const noexcept {
        return storage_;
    }

  private:
    void init_statements() {
        insert_reading_stmt_ =
            storage_.prepare("INSERT INTO readings (timestamp, noise_value, peak_value) VALUES (?, ?, ?);");
        insert_event_stmt_ =
            storage_.prepare("INSERT INTO events (timestamp, noise_value, peak_value) VALUES (?, ?, ?);");
        unsent_readings_stmt_ = storage_.prepare(
            "SELECT id, timestamp, noise_value, peak_value FROM readings WHERE sent=0 ORDER BY id ASC LIMIT ?;");
        unsent_events_stmt_ = storage_.prepare(
            "SELECT id, timestamp, noise_value, peak_value FROM events WHERE sent=0 ORDER BY id ASC LIMIT ?;");
        count_unsent_readings_stmt_ = storage_.prepare("SELECT COUNT(*) FROM readings WHERE sent=0;");
        count_unsent_events_stmt_ = storage_.prepare("SELECT COUNT(*) FROM events WHERE sent=0;");
    }

    mutable std::mutex mutex_;
    SqliteStorage storage_;

    // Cached statements for fast multi-threaded operations
    SqliteStmt insert_reading_stmt_;
    SqliteStmt insert_event_stmt_;
    SqliteStmt unsent_readings_stmt_;
    SqliteStmt unsent_events_stmt_;
    SqliteStmt count_unsent_readings_stmt_;
    SqliteStmt count_unsent_events_stmt_;
};

} // namespace muteq
