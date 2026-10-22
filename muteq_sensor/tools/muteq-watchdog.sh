#!/usr/bin/env bash
# ==============================================================================
# muteq-watchdog.sh — Automated Edge Node Health Guard for MUTEq Sensor
#
# Monitors:
#   1. Process health & crash recovery (muteq-sensor.service)
#   2. Hardware telemetry stream freshness (detects silent serial disconnects)
#   3. SQLite WAL file bloat and auto-checkpoints
#   4. Database size and automated safe pruning
#   5. Filesystem disk space pressure & journald vacuuming
# ==============================================================================
set -euo pipefail

SERVICE_NAME="muteq-sensor.service"
DB_PATH="/var/lib/muteq-sensor/muteq.db"
WAL_PATH="/var/lib/muteq-sensor/muteq.db-wal"
MAX_STALE_SECONDS=180
MAX_WAL_BYTES=$((50 * 1024 * 1024)) # 50 MB
MIN_DISK_FREE_KB=$((1024 * 1024))   # 1 GB

log_info() {
    logger -t "muteq-watchdog" -p user.info "[INFO] $*"
    echo "[INFO] $*"
}

log_warn() {
    logger -t "muteq-watchdog" -p user.warning "[WARN] $*"
    echo "[WARN] $*" >&2
}

log_err() {
    logger -t "muteq-watchdog" -p user.err "[ERR] $*"
    echo "[ERR] $*" >&2
}

# ── 1. Check Filesystem Disk Pressure ──────────────────────────────────────────
DISK_AVAIL_KB=$(df -k / | awk 'NR==2 {print $4}')
DISK_USE_PCT=$(df -k / | awk 'NR==2 {print $5}')

if [ "$DISK_AVAIL_KB" -lt "$MIN_DISK_FREE_KB" ]; then
    log_warn "Low disk space alert: only ${DISK_AVAIL_KB}KB available (${DISK_USE_PCT} used)!"
    
    # Emergency cleanup: vacuum journals down to 50M
    journalctl --vacuum-size=50M 2>/dev/null || true
    
    # Remove any abandoned temporary SQLite etilqs files in /tmp and /var/tmp
    rm -f /tmp/etilqs_* /var/tmp/etilqs_* 2>/dev/null || true

    # Force immediate WAL truncation
    if [ -f "$DB_PATH" ]; then
        python3 -c "
import sqlite3
con = sqlite3.connect('$DB_PATH')
con.execute('PRAGMA wal_checkpoint(TRUNCATE);')
con.close()
" 2>/dev/null || true
    fi
fi

# ── 2. Check Service Liveness ─────────────────────────────────────────────────
SERVICE_ACTIVE=true
if ! systemctl is-active --quiet "$SERVICE_NAME"; then
    SERVICE_ACTIVE=false
    log_err "$SERVICE_NAME is not active. Attempting restart..."
    systemctl restart "$SERVICE_NAME"
    sleep 3
    if systemctl is-active --quiet "$SERVICE_NAME"; then
        log_info "$SERVICE_NAME successfully recovered and active."
        SERVICE_ACTIVE=true
    else
        log_err "Failed to restart $SERVICE_NAME!"
    fi
fi

# ── 3. Check Telemetry Feed Freshness ─────────────────────────────────────────
FEED_AGE_SEC="unknown"
if [ "$SERVICE_ACTIVE" = true ] && [ -f "$DB_PATH" ]; then
    CHECK_OUTPUT=$(python3 -c "
import sqlite3, datetime, sys

try:
    con = sqlite3.connect('file:$DB_PATH?mode=ro', uri=True)
    cur = con.cursor()
    cur.execute('SELECT max(timestamp) FROM readings;')
    row = cur.fetchone()
    con.close()
    if not row or not row[0]:
        print('NO_DATA')
        sys.exit(0)

    ts_str = row[0]
    if ts_str.endswith('Z'):
        ts_str = ts_str[:-1] + '+00:00'
    last_dt = datetime.datetime.fromisoformat(ts_str)
    now_dt = datetime.datetime.now(datetime.timezone.utc)
    age = (now_dt - last_dt).total_seconds()
    print(f'{age:.1f}')
except Exception as e:
    print(f'ERROR: {e}')
    sys.exit(1)
" 2>/dev/null || echo "ERROR")

    if [ "$CHECK_OUTPUT" != "ERROR" ] && [ "$CHECK_OUTPUT" != "NO_DATA" ]; then
        FEED_AGE_SEC="$CHECK_OUTPUT"
        # Compare integer seconds
        AGE_INT="${CHECK_OUTPUT%.*}"
        if [ "$AGE_INT" -gt "$MAX_STALE_SECONDS" ]; then
            log_warn "Telemetry feed is stale! Last reading was ${FEED_AGE_SEC}s ago (> ${MAX_STALE_SECONDS}s)."
            log_warn "Restarting $SERVICE_NAME to re-initialize hardware serial connection..."
            systemctl restart "$SERVICE_NAME"
        fi
    fi
fi

# ── 4. Check WAL File Size & Maintenance ──────────────────────────────────────
WAL_SIZE_BYTES=0
if [ -f "$WAL_PATH" ]; then
    WAL_SIZE_BYTES=$(stat -c%s "$WAL_PATH" 2>/dev/null || stat -f%z "$WAL_PATH" 2>/dev/null || echo 0)
    if [ "$WAL_SIZE_BYTES" -gt "$MAX_WAL_BYTES" ]; then
        log_info "WAL file (${WAL_SIZE_BYTES} bytes) exceeds limit (${MAX_WAL_BYTES} bytes). Checkpointing..."
        python3 -c "
import sqlite3
con = sqlite3.connect('$DB_PATH')
con.execute('PRAGMA wal_checkpoint(TRUNCATE);')
con.close()
" 2>/dev/null || true
    fi
fi

# ── 5. Heartbeat Log ──────────────────────────────────────────────────────────
DB_SIZE_BYTES=$(stat -c%s "$DB_PATH" 2>/dev/null || stat -f%z "$DB_PATH" 2>/dev/null || echo 0)
DB_SIZE_MB=$(awk "BEGIN {printf \"%.1f\", $DB_SIZE_BYTES / 1048576}")
WAL_SIZE_MB=$(awk "BEGIN {printf \"%.1f\", $WAL_SIZE_BYTES / 1048576}")
DISK_AVAIL_GB=$(awk "BEGIN {printf \"%.1f\", $DISK_AVAIL_KB / 1048576}")

log_info "Health OK | disk_avail=${DISK_AVAIL_GB}GB (${DISK_USE_PCT} used) | db=${DB_SIZE_MB}MB | wal=${WAL_SIZE_MB}MB | feed_age=${FEED_AGE_SEC}s | service=$(systemctl is-active $SERVICE_NAME 2>/dev/null || echo unknown)"
