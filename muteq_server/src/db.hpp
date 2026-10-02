//
//  db.hpp
//  muteq_server
//
//  Thread-safe SQLite storage engine for MUTEq Dashboard Server.
//  Replicates and optimizes all queries from mute/dashboard/server.py.
//

#pragma once

#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace server {

/// Parse ISO 8601 string to unix timestamp in seconds
inline int64_t iso_to_unix(std::string_view iso) {
    if (iso.size() < 19) return 0;
    std::tm tm{};
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (sscanf(iso.data(), "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) < 6) {
        return 0;
    }
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
#if defined(_WIN32)
    return static_cast<int64_t>(_mkgmtime(&tm));
#else
    return static_cast<int64_t>(timegm(&tm));
#endif
}

/// Convert unix timestamp in seconds to ISO 8601 UTC string
inline std::string unix_to_iso(int64_t ts) {
    std::time_t sec = static_cast<std::time_t>(ts);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &sec);
#else
    gmtime_r(&sec, &tm_buf);
#endif
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
                       tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                       tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
}

struct ReadingPoint {
    int64_t time{0}; // unix seconds
    float value{0.0f};
};

struct EventPoint {
    int64_t time{0}; // unix seconds
    float noise{0.0f};
    float peak{0.0f};
};

struct ReadingInput {
    std::string timestamp;
    float noise_value{0.0f};
    float peak_value{0.0f};
};

struct EventInput {
    std::string timestamp;
    float noise_value{0.0f};
    float peak_value{0.0f};
};

struct DailyStatPoint {
    std::string day;
    double avg_noise{0.0};
    double peak_noise{0.0};
    int64_t event_count{0};
};

struct StatsResult {
    std::optional<float> latest;
    std::optional<float> peak;
    int64_t event_count{0};
    struct {
        std::optional<float> p50;
        std::optional<float> p90;
        std::optional<float> p99;
    } percentiles;
    struct {
        std::vector<std::string> labels;
        std::vector<int64_t> counts;
    } histogram;
    std::vector<int64_t> heatmap; // 24 hours
    std::vector<DailyStatPoint> daily_stats;
};

class ServerDatabase {
public:
    explicit ServerDatabase(const std::string& db_path) : db_path_(db_path) {
        constexpr int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
        int rc = ::sqlite3_open_v2(db_path.c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            std::string err = db_ ? ::sqlite3_errmsg(db_) : "Failed opening SQLite database";
            if (db_) ::sqlite3_close_v2(db_);
            throw std::runtime_error(err);
        }

        init_schema();
    }

    ~ServerDatabase() {
        if (db_) {
            ::sqlite3_close_v2(db_);
            db_ = nullptr;
        }
    }

    ServerDatabase(const ServerDatabase&) = delete;
    ServerDatabase& operator=(const ServerDatabase&) = delete;

    /// Ingest a batch of readings and events atomically
    std::pair<size_t, size_t> ingest(const std::vector<ReadingInput>& readings, const std::vector<EventInput>& events) {
        std::lock_guard<std::mutex> lock(mutex_);
        char* err_msg = nullptr;
        ::sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &err_msg);

        sqlite3_stmt* r_stmt = nullptr;
        ::sqlite3_prepare_v2(db_, "INSERT INTO readings (timestamp, noise_value, peak_value) VALUES (?, ?, ?);", -1, &r_stmt, nullptr);

        for (const auto& r : readings) {
            ::sqlite3_reset(r_stmt);
            ::sqlite3_bind_text(r_stmt, 1, r.timestamp.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_double(r_stmt, 2, static_cast<double>(r.noise_value));
            ::sqlite3_bind_double(r_stmt, 3, static_cast<double>(r.peak_value));
            ::sqlite3_step(r_stmt);
        }
        ::sqlite3_finalize(r_stmt);

        sqlite3_stmt* e_stmt = nullptr;
        ::sqlite3_prepare_v2(db_, "INSERT INTO events (timestamp, noise_value, peak_value) VALUES (?, ?, ?);", -1, &e_stmt, nullptr);

        for (const auto& e : events) {
            ::sqlite3_reset(e_stmt);
            ::sqlite3_bind_text(e_stmt, 1, e.timestamp.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_double(e_stmt, 2, static_cast<double>(e.noise_value));
            ::sqlite3_bind_double(e_stmt, 3, static_cast<double>(e.peak_value));
            ::sqlite3_step(e_stmt);
        }
        ::sqlite3_finalize(e_stmt);

        ::sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        return {readings.size(), events.size()};
    }

    /// Query readings with optional time window and downsampling
    std::vector<ReadingPoint> get_readings(std::optional<int64_t> from_ts, std::optional<int64_t> to_ts, size_t limit = 2000) {
        std::lock_guard<std::mutex> lock(mutex_);
        constexpr int max_fetch = 20000;
        std::vector<ReadingPoint> raw_rows;
        raw_rows.reserve(std::min(limit * 2, size_t{max_fetch}));

        std::string query;
        sqlite3_stmt* stmt = nullptr;

        std::string from_iso = from_ts ? unix_to_iso(*from_ts) : "";
        std::string to_iso = to_ts ? unix_to_iso(*to_ts) : "";

        if (from_ts && to_ts) {
            query = "SELECT timestamp, noise_value FROM readings WHERE timestamp >= ? AND timestamp <= ? ORDER BY timestamp ASC LIMIT ?;";
            ::sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
            ::sqlite3_bind_text(stmt, 1, from_iso.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_text(stmt, 2, to_iso.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_int(stmt, 3, max_fetch);
        } else if (from_ts) {
            query = "SELECT timestamp, noise_value FROM readings WHERE timestamp >= ? ORDER BY timestamp ASC LIMIT ?;";
            ::sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
            ::sqlite3_bind_text(stmt, 1, from_iso.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_int(stmt, 2, max_fetch);
        } else if (to_ts) {
            query = "SELECT timestamp, noise_value FROM readings WHERE timestamp <= ? ORDER BY timestamp DESC LIMIT ?;";
            ::sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
            ::sqlite3_bind_text(stmt, 1, to_iso.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_int(stmt, 2, max_fetch);
        } else {
            query = "SELECT timestamp, noise_value FROM readings ORDER BY timestamp DESC LIMIT ?;";
            ::sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
            ::sqlite3_bind_int(stmt, 1, max_fetch);
        }

        while (::sqlite3_step(stmt) == SQLITE_ROW) {
            const char* ts_str = reinterpret_cast<const char*>(::sqlite3_column_text(stmt, 0));
            double val = ::sqlite3_column_double(stmt, 1);
            if (ts_str) {
                raw_rows.push_back({.time = iso_to_unix(ts_str), .value = static_cast<float>(val)});
            }
        }
        ::sqlite3_finalize(stmt);

        // Reverse if query was ORDER BY DESC (scroll-back or default latest)
        if (to_ts || (!from_ts && !to_ts)) {
            std::reverse(raw_rows.begin(), raw_rows.end());
        }

        // Downsample if raw rows exceed requested limit
        if (raw_rows.size() <= limit || limit == 0) {
            return raw_rows;
        }

        std::vector<ReadingPoint> downsampled;
        downsampled.reserve(limit);
        size_t step = raw_rows.size() / limit;
        if (step == 0) step = 1;

        for (size_t i = 0; i < raw_rows.size(); i += step) {
            downsampled.push_back(raw_rows[i]);
            if (downsampled.size() >= limit) break;
        }

        return downsampled;
    }

    /// Query threshold events
    std::vector<EventPoint> get_events(std::optional<int64_t> from_ts, std::optional<int64_t> to_ts, size_t limit = 500) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<EventPoint> results;
        results.reserve(limit);

        std::string query;
        sqlite3_stmt* stmt = nullptr;
        std::string from_iso = from_ts ? unix_to_iso(*from_ts) : "";
        std::string to_iso = to_ts ? unix_to_iso(*to_ts) : "";

        if (from_ts && to_ts) {
            query = "SELECT timestamp, noise_value, peak_value FROM events WHERE timestamp >= ? AND timestamp <= ? ORDER BY timestamp DESC LIMIT ?;";
            ::sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
            ::sqlite3_bind_text(stmt, 1, from_iso.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_text(stmt, 2, to_iso.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_int(stmt, 3, static_cast<int>(limit));
        } else if (from_ts) {
            query = "SELECT timestamp, noise_value, peak_value FROM events WHERE timestamp >= ? ORDER BY timestamp DESC LIMIT ?;";
            ::sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
            ::sqlite3_bind_text(stmt, 1, from_iso.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_int(stmt, 2, static_cast<int>(limit));
        } else if (to_ts) {
            query = "SELECT timestamp, noise_value, peak_value FROM events WHERE timestamp <= ? ORDER BY timestamp DESC LIMIT ?;";
            ::sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
            ::sqlite3_bind_text(stmt, 1, to_iso.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_int(stmt, 2, static_cast<int>(limit));
        } else {
            query = "SELECT timestamp, noise_value, peak_value FROM events ORDER BY timestamp DESC LIMIT ?;";
            ::sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
            ::sqlite3_bind_int(stmt, 1, static_cast<int>(limit));
        }

        while (::sqlite3_step(stmt) == SQLITE_ROW) {
            const char* ts_str = reinterpret_cast<const char*>(::sqlite3_column_text(stmt, 0));
            double noise = ::sqlite3_column_double(stmt, 1);
            double peak = ::sqlite3_column_double(stmt, 2);
            if (ts_str) {
                results.push_back({
                    .time = iso_to_unix(ts_str),
                    .noise = static_cast<float>(noise),
                    .peak = static_cast<float>(peak)
                });
            }
        }
        ::sqlite3_finalize(stmt);
        return results;
    }

    /// Compute statistics, percentiles, histogram, heatmap, and daily stats
    StatsResult get_stats(std::optional<int64_t> from_ts) {
        std::lock_guard<std::mutex> lock(mutex_);
        StatsResult res;

        auto now = std::chrono::system_clock::now();
        int64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

        int64_t window_from = from_ts.value_or(now_ts - 86400); // Default 24h
        std::string from_iso = unix_to_iso(window_from);
        std::string week_ago_iso = unix_to_iso(now_ts - 7 * 86400);
        std::string month_ago_iso = unix_to_iso(now_ts - 30 * 86400);

        // 1. All noise values in the window
        std::vector<float> values;
        sqlite3_stmt* stmt = nullptr;
        ::sqlite3_prepare_v2(db_, "SELECT noise_value FROM readings WHERE timestamp >= ? ORDER BY timestamp ASC;", -1, &stmt, nullptr);
        ::sqlite3_bind_text(stmt, 1, from_iso.c_str(), -1, SQLITE_TRANSIENT);
        while (::sqlite3_step(stmt) == SQLITE_ROW) {
            values.push_back(static_cast<float>(::sqlite3_column_double(stmt, 0)));
        }
        ::sqlite3_finalize(stmt);

        // 2. Latest single reading
        ::sqlite3_prepare_v2(db_, "SELECT noise_value FROM readings ORDER BY timestamp DESC LIMIT 1;", -1, &stmt, nullptr);
        if (::sqlite3_step(stmt) == SQLITE_ROW) {
            res.latest = static_cast<float>(::sqlite3_column_double(stmt, 0));
        }
        ::sqlite3_finalize(stmt);

        // 3. Peak in window
        if (!values.empty()) {
            res.peak = *std::max_element(values.begin(), values.end());
        }

        // 4. Event count in window
        ::sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM events WHERE timestamp >= ?;", -1, &stmt, nullptr);
        ::sqlite3_bind_text(stmt, 1, from_iso.c_str(), -1, SQLITE_TRANSIENT);
        if (::sqlite3_step(stmt) == SQLITE_ROW) {
            res.event_count = ::sqlite3_column_int64(stmt, 0);
        }
        ::sqlite3_finalize(stmt);

        // 5. Percentiles
        if (!values.empty()) {
            std::vector<float> sorted_vals = values;
            std::sort(sorted_vals.begin(), sorted_vals.end());
            auto calc_pct = [&](int p) -> float {
                size_t idx = std::min(static_cast<size_t>(sorted_vals.size() * static_cast<size_t>(p) / 100), sorted_vals.size() - 1);
                return std::round(sorted_vals[idx] * 10.0f) / 10.0f;
            };
            res.percentiles.p50 = calc_pct(50);
            res.percentiles.p90 = calc_pct(90);
            res.percentiles.p99 = calc_pct(99);
        }

        // 6. Histogram: 5 dB bins from 40 to 100 plus >= 100
        std::vector<int> bin_starts;
        for (int b = 40; b < 100; b += 5) bin_starts.push_back(b);

        for (int b : bin_starts) {
            res.histogram.labels.push_back(std::format("{}-{}", b, b + 5));
        }
        res.histogram.labels.push_back("≥100");
        res.histogram.counts.assign(res.histogram.labels.size(), 0);

        for (float v : values) {
            if (v < 40.0f) continue;
            if (v >= 100.0f) {
                res.histogram.counts.back()++;
            } else {
                size_t idx = static_cast<size_t>((v - 40.0f) / 5.0f);
                if (idx < bin_starts.size()) {
                    res.histogram.counts[idx]++;
                }
            }
        }

        // 7. Hourly Heatmap (last 7 days, 24 buckets)
        res.heatmap.assign(24, 0);
        ::sqlite3_prepare_v2(db_,
            "SELECT CAST(strftime('%H', timestamp, 'localtime') AS INTEGER) AS hour, COUNT(*) AS count "
            "FROM events WHERE timestamp >= ? GROUP BY hour ORDER BY hour;", -1, &stmt, nullptr);
        ::sqlite3_bind_text(stmt, 1, week_ago_iso.c_str(), -1, SQLITE_TRANSIENT);
        while (::sqlite3_step(stmt) == SQLITE_ROW) {
            int h = ::sqlite3_column_int(stmt, 0);
            int64_t cnt = ::sqlite3_column_int64(stmt, 1);
            if (h >= 0 && h < 24) {
                res.heatmap[static_cast<size_t>(h)] = cnt;
            }
        }
        ::sqlite3_finalize(stmt);

        // 8. Daily stats (last 30 days)
        ::sqlite3_prepare_v2(db_,
            "SELECT date(timestamp, 'localtime') AS day, "
            "  ROUND(AVG(noise_value), 1) AS avg_noise, "
            "  ROUND(MAX(noise_value), 1) AS peak_noise "
            "FROM readings WHERE timestamp >= ? GROUP BY day ORDER BY day DESC LIMIT 30;", -1, &stmt, nullptr);
        ::sqlite3_bind_text(stmt, 1, month_ago_iso.c_str(), -1, SQLITE_TRANSIENT);
        while (::sqlite3_step(stmt) == SQLITE_ROW) {
            const char* d_str = reinterpret_cast<const char*>(::sqlite3_column_text(stmt, 0));
            double avg = ::sqlite3_column_double(stmt, 1);
            double peak = ::sqlite3_column_double(stmt, 2);
            if (d_str) {
                res.daily_stats.push_back({
                    .day = d_str,
                    .avg_noise = avg,
                    .peak_noise = peak,
                    .event_count = 0
                });
            }
        }
        ::sqlite3_finalize(stmt);

        // Fetch event counts per day
        ::sqlite3_prepare_v2(db_,
            "SELECT date(timestamp, 'localtime') AS day, COUNT(*) AS event_count "
            "FROM events WHERE timestamp >= ? GROUP BY day;", -1, &stmt, nullptr);
        ::sqlite3_bind_text(stmt, 1, month_ago_iso.c_str(), -1, SQLITE_TRANSIENT);
        while (::sqlite3_step(stmt) == SQLITE_ROW) {
            const char* d_str = reinterpret_cast<const char*>(::sqlite3_column_text(stmt, 0));
            int64_t count = ::sqlite3_column_int64(stmt, 1);
            if (d_str) {
                for (auto& row : res.daily_stats) {
                    if (row.day == d_str) {
                        row.event_count = count;
                        break;
                    }
                }
            }
        }
        ::sqlite3_finalize(stmt);

        return res;
    }

private:
    void init_schema() {
        char* err_msg = nullptr;
        ::sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
        ::sqlite3_exec(db_, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
        ::sqlite3_exec(db_, "PRAGMA busy_timeout = 5000;", nullptr, nullptr, nullptr);

        const char* schema = 
            "CREATE TABLE IF NOT EXISTS readings ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    timestamp TEXT NOT NULL,"
            "    noise_value REAL NOT NULL,"
            "    peak_value REAL"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings (timestamp);"
            "CREATE TABLE IF NOT EXISTS events ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    timestamp TEXT NOT NULL,"
            "    noise_value REAL NOT NULL,"
            "    peak_value REAL"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_events_ts ON events (timestamp);";

        if (::sqlite3_exec(db_, schema, nullptr, nullptr, &err_msg) != SQLITE_OK) {
            std::string err = err_msg ? err_msg : "Schema init failed";
            ::sqlite3_free(err_msg);
            throw std::runtime_error(err);
        }
    }

    std::string db_path_;
    sqlite3* db_{nullptr};
    std::mutex mutex_;
};

} // namespace server
