//
//  sqlite_storage.hpp
//  muteq_sensor
//
//  Direct SQLite C API RAII wrapper for embedded telemetry storage.
//  Configured for WAL mode, low flash write-wear, and offline ring buffering.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <span>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace muteq {

/// Exception thrown for SQLite operation errors with full error message and return code.
class SqliteError : public std::runtime_error {
  public:
    SqliteError(std::string_view context, int error_code, std::string_view sqlite_msg = {})
        : std::runtime_error(std::format("{}: {} (code {})", context,
                                         sqlite_msg.empty() ? ::sqlite3_errstr(error_code) : sqlite_msg, error_code)),
          code_(error_code) {
    }

    [[nodiscard]] int code() const noexcept {
        return code_;
    }

  private:
    int code_;
};

/// RAII wrapper for sqlite3_stmt*
class SqliteStmt {
  public:
    SqliteStmt() noexcept = default;
    explicit SqliteStmt(sqlite3_stmt* stmt) noexcept : stmt_(stmt) {
    }

    ~SqliteStmt() {
        reset_and_finalize();
    }

    // Move-only semantics
    SqliteStmt(const SqliteStmt&) = delete;
    SqliteStmt& operator=(const SqliteStmt&) = delete;

    SqliteStmt(SqliteStmt&& other) noexcept : stmt_(std::exchange(other.stmt_, nullptr)) {
    }

    SqliteStmt& operator=(SqliteStmt&& other) noexcept {
        if (this != &other) {
            reset_and_finalize();
            stmt_ = std::exchange(other.stmt_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] sqlite3_stmt* get() const noexcept {
        return stmt_;
    }
    explicit operator bool() const noexcept {
        return stmt_ != nullptr;
    }

    void reset() {
        if (stmt_) {
            ::sqlite3_reset(stmt_);
            ::sqlite3_clear_bindings(stmt_);
        }
    }

    int step() {
        return stmt_ ? ::sqlite3_step(stmt_) : SQLITE_MISUSE;
    }

    void execute_done() {
        int rc = step();
        if (rc != SQLITE_DONE) {
            sqlite3* db = ::sqlite3_db_handle(stmt_);
            throw SqliteError("Failed executing statement", rc, db ? ::sqlite3_errmsg(db) : "");
        }
    }

    // Bind parameters (1-based index)
    void bind_int64(int idx, int64_t val) {
        int rc = ::sqlite3_bind_int64(stmt_, idx, val);
        if (rc != SQLITE_OK)
            throw_bind_error(rc);
    }

    void bind_double(int idx, double val) {
        int rc = ::sqlite3_bind_double(stmt_, idx, val);
        if (rc != SQLITE_OK)
            throw_bind_error(rc);
    }

    void bind_text(int idx, std::string_view val) {
        int rc = ::sqlite3_bind_text(stmt_, idx, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
        if (rc != SQLITE_OK)
            throw_bind_error(rc);
    }

    void bind_null(int idx) {
        int rc = ::sqlite3_bind_null(stmt_, idx);
        if (rc != SQLITE_OK)
            throw_bind_error(rc);
    }

    // Column accessors (0-based index)
    [[nodiscard]] int64_t column_int64(int col) const noexcept {
        return ::sqlite3_column_int64(stmt_, col);
    }

    [[nodiscard]] double column_double(int col) const noexcept {
        return ::sqlite3_column_double(stmt_, col);
    }

    [[nodiscard]] int column_int(int col) const noexcept {
        return ::sqlite3_column_int(stmt_, col);
    }

    [[nodiscard]] std::string_view column_text(int col) const noexcept {
        const auto* txt = reinterpret_cast<const char*>(::sqlite3_column_text(stmt_, col));
        int bytes = ::sqlite3_column_bytes(stmt_, col);
        return txt ? std::string_view(txt, static_cast<size_t>(bytes)) : std::string_view{};
    }

  private:
    void reset_and_finalize() noexcept {
        if (stmt_) {
            ::sqlite3_finalize(stmt_);
            stmt_ = nullptr;
        }
    }

    void throw_bind_error(int rc) const {
        sqlite3* db = ::sqlite3_db_handle(stmt_);
        throw SqliteError("Failed binding parameter", rc, db ? ::sqlite3_errmsg(db) : "");
    }

    sqlite3_stmt* stmt_{nullptr};
};

/// Record for reading and event tables (matching mute/dashboard and mute/sensor)
struct TelemetryRecord {
    int64_t id{0};
    std::string timestamp; // ISO 8601 UTC
    float noise_value{0.0f};
    float peak_value{0.0f};
    bool sent{false};
};

/// Lightweight representation of a stored reading record (spl_records fallback)
struct SplRecord {
    int64_t id{0};
    int64_t timestamp_ms{0};
    float db_a{0.0f};
    bool synced{false};
};

/// Input structure for batch insertions
struct SplRecordInput {
    int64_t timestamp_ms{0};
    float db_a{0.0f};
};

/// RAII transaction guard using BEGIN IMMEDIATE to avoid WAL busy-lock contention
class Transaction {
  public:
    explicit Transaction(sqlite3* db) : db_(db) {
        char* err_msg = nullptr;
        if (::sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
            std::string err = err_msg ? err_msg : "Unknown error";
            ::sqlite3_free(err_msg);
            throw std::runtime_error(std::format("Failed to begin transaction: {}", err));
        }
        active_ = true;
    }

    ~Transaction() {
        if (active_ && db_) {
            ::sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    // Move-only
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Transaction(Transaction&& other) noexcept
        : db_(std::exchange(other.db_, nullptr)), active_(std::exchange(other.active_, false)) {
    }

    Transaction& operator=(Transaction&& other) noexcept {
        if (this != &other) {
            if (active_ && db_) {
                ::sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
            db_ = std::exchange(other.db_, nullptr);
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    void commit() {
        if (!active_ || !db_)
            return;
        char* err_msg = nullptr;
        int rc = ::sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string err = err_msg ? err_msg : "Unknown error";
            ::sqlite3_free(err_msg);
            throw std::runtime_error(std::format("Failed to commit transaction: {}", err));
        }
        active_ = false;
    }

    void rollback() {
        if (!active_ || !db_)
            return;
        ::sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        active_ = false;
    }

  private:
    sqlite3* db_{nullptr};
    bool active_{false};
};

/// High-performance, thin RAII SQLite storage engine
class SqliteStorage {
  public:
    explicit SqliteStorage(std::string_view db_path) {
        const std::string path_str(db_path);
        constexpr int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;

        int rc = ::sqlite3_open_v2(path_str.c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            std::string msg = db_ ? ::sqlite3_errmsg(db_) : ::sqlite3_errstr(rc);
            if (db_) {
                ::sqlite3_close_v2(db_);
                db_ = nullptr;
            }
            throw SqliteError(std::format("Failed to open SQLite database at '{}'", path_str), rc, msg);
        }

        init_engine();
    }

    ~SqliteStorage() {
        close();
    }

    // Move-only
    SqliteStorage(const SqliteStorage&) = delete;
    SqliteStorage& operator=(const SqliteStorage&) = delete;

    SqliteStorage(SqliteStorage&& other) noexcept
        : db_(std::exchange(other.db_, nullptr)), insert_reading_stmt_(std::move(other.insert_reading_stmt_)),
          insert_event_stmt_(std::move(other.insert_event_stmt_)),
          fetch_unsent_readings_stmt_(std::move(other.fetch_unsent_readings_stmt_)),
          fetch_unsent_events_stmt_(std::move(other.fetch_unsent_events_stmt_)),
          mark_reading_sent_stmt_(std::move(other.mark_reading_sent_stmt_)),
          mark_event_sent_stmt_(std::move(other.mark_event_sent_stmt_)), insert_stmt_(std::move(other.insert_stmt_)),
          fetch_unsynced_stmt_(std::move(other.fetch_unsynced_stmt_)),
          mark_synced_up_to_stmt_(std::move(other.mark_synced_up_to_stmt_)),
          purge_synced_stmt_(std::move(other.purge_synced_stmt_)),
          trim_records_stmt_(std::move(other.trim_records_stmt_)),
          count_unsynced_stmt_(std::move(other.count_unsynced_stmt_)),
          count_total_stmt_(std::move(other.count_total_stmt_)) {
    }

    SqliteStorage& operator=(SqliteStorage&& other) noexcept {
        if (this != &other) {
            close();
            db_ = std::exchange(other.db_, nullptr);
            insert_reading_stmt_ = std::move(other.insert_reading_stmt_);
            insert_event_stmt_ = std::move(other.insert_event_stmt_);
            fetch_unsent_readings_stmt_ = std::move(other.fetch_unsent_readings_stmt_);
            fetch_unsent_events_stmt_ = std::move(other.fetch_unsent_events_stmt_);
            mark_reading_sent_stmt_ = std::move(other.mark_reading_sent_stmt_);
            mark_event_sent_stmt_ = std::move(other.mark_event_sent_stmt_);
            insert_stmt_ = std::move(other.insert_stmt_);
            fetch_unsynced_stmt_ = std::move(other.fetch_unsynced_stmt_);
            mark_synced_up_to_stmt_ = std::move(other.mark_synced_up_to_stmt_);
            purge_synced_stmt_ = std::move(other.purge_synced_stmt_);
            trim_records_stmt_ = std::move(other.trim_records_stmt_);
            count_unsynced_stmt_ = std::move(other.count_unsynced_stmt_);
            count_total_stmt_ = std::move(other.count_total_stmt_);
        }
        return *this;
    }

    /// Prepare any custom SQL statement against this database
    [[nodiscard]] SqliteStmt prepare(std::string_view sql) const {
        sqlite3_stmt* raw_stmt = nullptr;
        int rc = ::sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &raw_stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw SqliteError(std::format("Failed to prepare statement '{}'", sql), rc, ::sqlite3_errmsg(db_));
        }
        return SqliteStmt(raw_stmt);
    }

    /// Execute a non-query SQL string directly
    void exec(std::string_view sql) const {
        char* err_msg = nullptr;
        int rc = ::sqlite3_exec(db_, sql.data(), nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string err = err_msg ? err_msg : "Unknown error";
            ::sqlite3_free(err_msg);
            throw SqliteError(std::format("SQL execution failed on '{}'", sql), rc, err);
        }
    }

    /// Begin an explicit RAII transaction
    [[nodiscard]] Transaction begin_transaction() {
        return Transaction(db_);
    }

    // --- Dashboard/Ingest API (readings and events tables) ---

    /// Insert one SPL reading (called periodically with smoothed noise and peak value)
    void write_reading(std::string_view timestamp_iso, float noise_value, float peak_value) {
        insert_reading_stmt_.reset();
        insert_reading_stmt_.bind_text(1, timestamp_iso);
        insert_reading_stmt_.bind_double(2, static_cast<double>(noise_value));
        insert_reading_stmt_.bind_double(3, static_cast<double>(peak_value));
        insert_reading_stmt_.execute_done();
    }

    /// Insert a threshold-breach event (noise >= threshold)
    void write_event(std::string_view timestamp_iso, float noise_value, float peak_value) {
        insert_event_stmt_.reset();
        insert_event_stmt_.bind_text(1, timestamp_iso);
        insert_event_stmt_.bind_double(2, static_cast<double>(noise_value));
        insert_event_stmt_.bind_double(3, static_cast<double>(peak_value));
        insert_event_stmt_.execute_done();
    }

    /// Fetch unsent readings capped at `limit` (oldest first)
    [[nodiscard]] std::vector<TelemetryRecord> get_unsent_readings(size_t limit = 5000) {
        std::vector<TelemetryRecord> records;
        records.reserve(std::min(limit, size_t{5000}));

        fetch_unsent_readings_stmt_.reset();
        fetch_unsent_readings_stmt_.bind_int64(1, static_cast<int64_t>(limit));

        while (fetch_unsent_readings_stmt_.step() == SQLITE_ROW) {
            records.push_back(
                TelemetryRecord{.id = fetch_unsent_readings_stmt_.column_int64(0),
                                .timestamp = std::string(fetch_unsent_readings_stmt_.column_text(1)),
                                .noise_value = static_cast<float>(fetch_unsent_readings_stmt_.column_double(2)),
                                .peak_value = static_cast<float>(fetch_unsent_readings_stmt_.column_double(3)),
                                .sent = false});
        }
        return records;
    }

    /// Fetch unsent events capped at `limit` (oldest first)
    [[nodiscard]] std::vector<TelemetryRecord> get_unsent_events(size_t limit = 2000) {
        std::vector<TelemetryRecord> events;
        events.reserve(std::min(limit, size_t{2000}));

        fetch_unsent_events_stmt_.reset();
        fetch_unsent_events_stmt_.bind_int64(1, static_cast<int64_t>(limit));

        while (fetch_unsent_events_stmt_.step() == SQLITE_ROW) {
            events.push_back(
                TelemetryRecord{.id = fetch_unsent_events_stmt_.column_int64(0),
                                .timestamp = std::string(fetch_unsent_events_stmt_.column_text(1)),
                                .noise_value = static_cast<float>(fetch_unsent_events_stmt_.column_double(2)),
                                .peak_value = static_cast<float>(fetch_unsent_events_stmt_.column_double(3)),
                                .sent = false});
        }
        return events;
    }

    /// Mark a list of reading IDs as sent
    void mark_readings_sent(std::span<const int64_t> ids) {
        if (ids.empty())
            return;
        auto tx = begin_transaction();
        for (int64_t id : ids) {
            mark_reading_sent_stmt_.reset();
            mark_reading_sent_stmt_.bind_int64(1, id);
            mark_reading_sent_stmt_.execute_done();
        }
        tx.commit();
    }

    /// Mark a list of event IDs as sent
    void mark_events_sent(std::span<const int64_t> ids) {
        if (ids.empty())
            return;
        auto tx = begin_transaction();
        for (int64_t id : ids) {
            mark_event_sent_stmt_.reset();
            mark_event_sent_stmt_.bind_int64(1, id);
            mark_event_sent_stmt_.execute_done();
        }
        tx.commit();
    }

    /// Delete sent data older than retain_days and unsent data older than unsent_retain_days
    void prune_old_data(std::string_view cutoff_sent_iso, std::string_view cutoff_unsent_iso) {
        auto tx = begin_transaction();
        auto prune_stmt = prepare("DELETE FROM readings WHERE timestamp < ? AND sent=1;");
        prune_stmt.bind_text(1, cutoff_sent_iso);
        prune_stmt.execute_done();

        prune_stmt = prepare("DELETE FROM events WHERE timestamp < ? AND sent=1;");
        prune_stmt.bind_text(1, cutoff_sent_iso);
        prune_stmt.execute_done();

        prune_stmt = prepare("DELETE FROM readings WHERE timestamp < ? AND sent=0;");
        prune_stmt.bind_text(1, cutoff_unsent_iso);
        prune_stmt.execute_done();

        prune_stmt = prepare("DELETE FROM events WHERE timestamp < ? AND sent=0;");
        prune_stmt.bind_text(1, cutoff_unsent_iso);
        prune_stmt.execute_done();
        tx.commit();
    }

    // --- Legacy spl_records helpers (for backward compatibility) ---

    void insert(int64_t timestamp_ms, float db_a) {
        insert_stmt_.reset();
        insert_stmt_.bind_int64(1, timestamp_ms);
        insert_stmt_.bind_double(2, static_cast<double>(db_a));
        insert_stmt_.execute_done();
    }

    void insert_batch(std::span<const SplRecordInput> records) {
        if (records.empty())
            return;
        auto tx = begin_transaction();
        for (const auto& rec : records) {
            insert_stmt_.reset();
            insert_stmt_.bind_int64(1, rec.timestamp_ms);
            insert_stmt_.bind_double(2, static_cast<double>(rec.db_a));
            insert_stmt_.execute_done();
        }
        tx.commit();
    }

    [[nodiscard]] std::vector<SplRecord> fetch_unsynced(size_t limit = 100) {
        std::vector<SplRecord> results;
        results.reserve(limit);

        fetch_unsynced_stmt_.reset();
        fetch_unsynced_stmt_.bind_int64(1, static_cast<int64_t>(limit));

        while (fetch_unsynced_stmt_.step() == SQLITE_ROW) {
            results.push_back(SplRecord{.id = fetch_unsynced_stmt_.column_int64(0),
                                        .timestamp_ms = fetch_unsynced_stmt_.column_int64(1),
                                        .db_a = static_cast<float>(fetch_unsynced_stmt_.column_double(2)),
                                        .synced = false});
        }
        return results;
    }

    void mark_synced(int64_t up_to_id) {
        mark_synced_up_to_stmt_.reset();
        mark_synced_up_to_stmt_.bind_int64(1, up_to_id);
        mark_synced_up_to_stmt_.execute_done();
    }

    size_t purge_synced_older_than(std::chrono::milliseconds max_age) {
        auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        int64_t cutoff = now_ms - max_age.count();

        purge_synced_stmt_.reset();
        purge_synced_stmt_.bind_int64(1, cutoff);
        purge_synced_stmt_.execute_done();
        return static_cast<size_t>(::sqlite3_changes(db_));
    }

    size_t trim_to_max_records(size_t max_records) {
        trim_records_stmt_.reset();
        trim_records_stmt_.bind_int64(1, static_cast<int64_t>(max_records));
        trim_records_stmt_.execute_done();
        return static_cast<size_t>(::sqlite3_changes(db_));
    }

    [[nodiscard]] size_t count_unsynced() {
        count_unsynced_stmt_.reset();
        if (count_unsynced_stmt_.step() == SQLITE_ROW) {
            return static_cast<size_t>(count_unsynced_stmt_.column_int64(0));
        }
        return 0;
    }

    [[nodiscard]] size_t count_total() {
        count_total_stmt_.reset();
        if (count_total_stmt_.step() == SQLITE_ROW) {
            return static_cast<size_t>(count_total_stmt_.column_int64(0));
        }
        return 0;
    }

    [[nodiscard]] sqlite3* raw_handle() const noexcept {
        return db_;
    }

  private:
    void init_engine() {
        // WAL journal mode and normal sync for SD card endurance
        exec("PRAGMA journal_mode = WAL;");
        exec("PRAGMA synchronous = NORMAL;");
        exec("PRAGMA busy_timeout = 5000;");

        // Readings & events schema (mirrors mute/sensor/app/db.py)
        exec("CREATE TABLE IF NOT EXISTS readings ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  timestamp TEXT NOT NULL,"
             "  noise_value REAL NOT NULL,"
             "  peak_value REAL,"
             "  sent INTEGER NOT NULL DEFAULT 0"
             ");"
             "CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings (timestamp);"
             "CREATE INDEX IF NOT EXISTS idx_readings_sent_ts ON readings (sent, timestamp);"
             "CREATE TABLE IF NOT EXISTS events ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  timestamp TEXT NOT NULL,"
             "  noise_value REAL NOT NULL,"
             "  peak_value REAL,"
             "  sent INTEGER NOT NULL DEFAULT 0"
             ");"
             "CREATE INDEX IF NOT EXISTS idx_events_ts ON events (timestamp);"
             "CREATE INDEX IF NOT EXISTS idx_events_sent_ts ON events (sent, timestamp);");

        // Prepare hot statements for readings & events
        insert_reading_stmt_ = prepare("INSERT INTO readings (timestamp, noise_value, peak_value) VALUES (?, ?, ?);");
        insert_event_stmt_ = prepare("INSERT INTO events (timestamp, noise_value, peak_value) VALUES (?, ?, ?);");
        fetch_unsent_readings_stmt_ = prepare(
            "SELECT id, timestamp, noise_value, peak_value FROM readings WHERE sent=0 ORDER BY id ASC LIMIT ?;");
        fetch_unsent_events_stmt_ =
            prepare("SELECT id, timestamp, noise_value, peak_value FROM events WHERE sent=0 ORDER BY id ASC LIMIT ?;");
        mark_reading_sent_stmt_ = prepare("UPDATE readings SET sent=1 WHERE id=?;");
        mark_event_sent_stmt_ = prepare("UPDATE events SET sent=1 WHERE id=?;");

        // Backward compatibility spl_records schema & statements
        exec("CREATE TABLE IF NOT EXISTS spl_records ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  timestamp_ms INTEGER NOT NULL,"
             "  db_a REAL NOT NULL,"
             "  synced INTEGER NOT NULL DEFAULT 0"
             ");"
             "CREATE INDEX IF NOT EXISTS idx_spl_unsynced ON spl_records (synced, id);");
        insert_stmt_ = prepare("INSERT INTO spl_records (timestamp_ms, db_a, synced) VALUES (?, ?, 0);");
        fetch_unsynced_stmt_ =
            prepare("SELECT id, timestamp_ms, db_a FROM spl_records WHERE synced = 0 ORDER BY id ASC LIMIT ?;");
        mark_synced_up_to_stmt_ = prepare("UPDATE spl_records SET synced = 1 WHERE id <= ? AND synced = 0;");
        purge_synced_stmt_ = prepare("DELETE FROM spl_records WHERE synced = 1 AND timestamp_ms < ?;");
        trim_records_stmt_ = prepare("DELETE FROM spl_records WHERE id IN ("
                                     "  SELECT id FROM spl_records ORDER BY id DESC LIMIT -1 OFFSET ?"
                                     ");");
        count_unsynced_stmt_ = prepare("SELECT COUNT(*) FROM spl_records WHERE synced = 0;");
        count_total_stmt_ = prepare("SELECT COUNT(*) FROM spl_records;");
    }

    void close() noexcept {
        insert_reading_stmt_ = SqliteStmt{};
        insert_event_stmt_ = SqliteStmt{};
        fetch_unsent_readings_stmt_ = SqliteStmt{};
        fetch_unsent_events_stmt_ = SqliteStmt{};
        mark_reading_sent_stmt_ = SqliteStmt{};
        mark_event_sent_stmt_ = SqliteStmt{};

        insert_stmt_ = SqliteStmt{};
        fetch_unsynced_stmt_ = SqliteStmt{};
        mark_synced_up_to_stmt_ = SqliteStmt{};
        purge_synced_stmt_ = SqliteStmt{};
        trim_records_stmt_ = SqliteStmt{};
        count_unsynced_stmt_ = SqliteStmt{};
        count_total_stmt_ = SqliteStmt{};

        if (db_) {
            ::sqlite3_close_v2(db_);
            db_ = nullptr;
        }
    }

    sqlite3* db_{nullptr};

    // Hot statement cache for readings and events
    SqliteStmt insert_reading_stmt_;
    SqliteStmt insert_event_stmt_;
    SqliteStmt fetch_unsent_readings_stmt_;
    SqliteStmt fetch_unsent_events_stmt_;
    SqliteStmt mark_reading_sent_stmt_;
    SqliteStmt mark_event_sent_stmt_;

    // Legacy statement cache
    SqliteStmt insert_stmt_;
    SqliteStmt fetch_unsynced_stmt_;
    SqliteStmt mark_synced_up_to_stmt_;
    SqliteStmt purge_synced_stmt_;
    SqliteStmt trim_records_stmt_;
    SqliteStmt count_unsynced_stmt_;
    SqliteStmt count_total_stmt_;
};

} // namespace muteq
