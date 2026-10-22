#!/usr/bin/env bash
# install.sh — Install and configure MUTEq C++ Sensor Node as a systemd service
#
# Usage (on Raspberry Pi):
#   chmod +x ./install.sh
#   ./install.sh
#
# This script:
#   1. Installs runtime dependencies (libsqlite3, libcurl, ca-certificates).
#   2. Installs the muteq_sensor binary to /usr/local/bin/muteq_sensor.
#   3. Sets up udev rules for the USB sound level meter.
#   4. Ensures /var/lib/muteq-sensor exists with correct user ownership.
#   5. Installs and starts the systemd service (muteq-sensor.service).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_BIN="/usr/local/bin/muteq_sensor"
SERVICE_NAME="muteq-sensor.service"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}"
ENV_FILE="/etc/muteq/sensor.env"
CONFIG_DIR="/var/lib/muteq-sensor"
RUN_USER="${SUDO_USER:-$(id -un)}"
RUN_GROUP="$(id -gn "$RUN_USER")"

echo "============================================="
echo "  MUTEq C++ Sensor Node Installer"
echo "  Service User: ${RUN_USER}:${RUN_GROUP}"
echo "============================================="

# ── 1. Locate binary ──────────────────────────────────────────────────────────
echo "▶ Checking binary source..."
SRC_BIN=""
CANDIDATE_BINS=(
  "$SCRIPT_DIR/bin/linux_arm64/muteq_sensor"
  "$SCRIPT_DIR/bin/muteq_sensor"
  "$SCRIPT_DIR/muteq_sensor"
  "/home/coder/muteq/muteq_sensor"
  "/home/coder/muteq_sensor/bin/linux_arm64/muteq_sensor"
)

for b in "${CANDIDATE_BINS[@]}"; do
  if [ -f "$b" ] && [ -x "$b" ]; then
    SRC_BIN="$b"
    break
  fi
done

if [ -z "$SRC_BIN" ]; then
  echo "❌ Error: Could not find built muteq_sensor executable."
  echo "   Please build the binary first (e.g. 'make cross-pi' or 'make build')."
  exit 1
fi
echo "   Found binary at: $SRC_BIN"

# ── 2. Install runtime dependencies ───────────────────────────────────────────
echo "▶ Installing system runtime libraries..."
if command -v apt-get &>/dev/null; then
  sudo apt-get update -qq
  sudo apt-get install -y --no-install-recommends \
    libsqlite3-0 \
    libcurl4 \
    ca-certificates
fi

# ── 3. Stop existing service if running ───────────────────────────────────────
if systemctl is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
  echo "▶ Stopping currently running $SERVICE_NAME..."
  sudo systemctl stop "$SERVICE_NAME"
fi

# ── 4. Install binary to /usr/local/bin ────────────────────────────────────────
echo "▶ Installing binary to $TARGET_BIN..."
sudo cp -f "$SRC_BIN" "$TARGET_BIN"
sudo chmod 755 "$TARGET_BIN"

# ── 5. Setup data directory & permissions ─────────────────────────────────────
echo "▶ Configuring data directory at $CONFIG_DIR..."
sudo mkdir -p "$CONFIG_DIR"
sudo chown -R "${RUN_USER}:${RUN_GROUP}" "$CONFIG_DIR"
if [ -f "$CONFIG_DIR/muteq.db" ]; then
  sudo chmod 664 "$CONFIG_DIR/muteq.db"* 2>/dev/null || true
fi

# ── 6. Setup udev rule for USB sound meter & serial converter ─────────────────
UDEV_RULE="/etc/udev/rules.d/99-muteq-sensor.rules"
echo "▶ Installing udev rules at $UDEV_RULE..."
sudo tee "$UDEV_RULE" > /dev/null <<'EOF'
# MUTEq sound meter chips (QinHeng CH340 serial & default USB HID meters)
SUBSYSTEM=="usb", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666", GROUP="dialout"
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666", GROUP="dialout"
SUBSYSTEM=="usb", ATTRS{idVendor}=="16c0", ATTRS{idProduct}=="05dc", MODE="0666", GROUP="dialout"
KERNEL=="ttyUSB*", MODE="0666", GROUP="dialout"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger

# Ensure service user is part of dialout group for serial access
if ! id -nG "$RUN_USER" | grep -qw "dialout"; then
  echo "▶ Adding $RUN_USER to dialout group..."
  sudo usermod -aG dialout "$RUN_USER"
fi

# ── 7. Configure environment file ─────────────────────────────────────────────
sudo mkdir -p /etc/muteq
echo "▶ Updating /etc/muteq/sensor.env..."
sudo tee "$ENV_FILE" > /dev/null <<EOF
MUTEQ_CONFIG_DIR=${CONFIG_DIR}
EOF
sudo chmod 644 "$ENV_FILE"

# ── 8. Find configuration file ────────────────────────────────────────────────
CONFIG_PATH=""
CANDIDATE_CONFIGS=(
  "/home/coder/muteq/client_config.json"
  "$CONFIG_DIR/client_config.json"
  "$CONFIG_DIR/config_client.json"
  "$SCRIPT_DIR/client_config.json"
)

for c in "${CANDIDATE_CONFIGS[@]}"; do
  if [ -f "$c" ]; then
    CONFIG_PATH="$c"
    break
  fi
done

if [ -n "$CONFIG_PATH" ]; then
  echo "   Using configuration file: $CONFIG_PATH"
  EXEC_CMD="$TARGET_BIN -c $CONFIG_PATH"
else
  echo "   No explicit config file found, binary will discover automatically."
  EXEC_CMD="$TARGET_BIN"
fi

# ── 9. Write systemd unit file ────────────────────────────────────────────────
echo "▶ Writing systemd unit to $SERVICE_FILE..."
sudo tee "$SERVICE_FILE" > /dev/null <<EOF
[Unit]
Description=MUTEq C++ Sensor Node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${RUN_USER}
Group=${RUN_GROUP}
SupplementaryGroups=dialout
WorkingDirectory=/home/coder
EnvironmentFile=-${ENV_FILE}
ExecStart=${EXEC_CMD}
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

# Graceful termination
TimeoutStopSec=15
KillSignal=SIGTERM

[Install]
WantedBy=multi-user.target
EOF

# ── 10. Enable and restart service ────────────────────────────────────────────
echo "▶ Reloading systemd daemon and starting $SERVICE_NAME..."
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE_NAME"
sudo systemctl restart "$SERVICE_NAME"

# ── 11. Configure Journald cap & install Watchdog Timer ─────────────────────────
echo "▶ Hardening systemd-journald size caps (max 100M)..."
if [ -f /etc/systemd/journald.conf ]; then
  if grep -q "^SystemMaxUse=" /etc/systemd/journald.conf; then
    sudo sed -i 's/^SystemMaxUse=.*/SystemMaxUse=100M/' /etc/systemd/journald.conf
  else
    echo "SystemMaxUse=100M" | sudo tee -a /etc/systemd/journald.conf > /dev/null
  fi
  sudo systemctl restart systemd-journald 2>/dev/null || true
fi

echo "▶ Installing MUTEq watchdog..."
WATCHDOG_SCRIPT_SRC="$SCRIPT_DIR/tools/muteq-watchdog.sh"
if [ -f "$WATCHDOG_SCRIPT_SRC" ]; then
  sudo cp -f "$WATCHDOG_SCRIPT_SRC" /usr/local/bin/muteq-watchdog.sh
  sudo chmod 755 /usr/local/bin/muteq-watchdog.sh
fi

if [ -f "$SCRIPT_DIR/tools/muteq-watchdog.service" ] && [ -f "$SCRIPT_DIR/tools/muteq-watchdog.timer" ]; then
  sudo cp -f "$SCRIPT_DIR/tools/muteq-watchdog.service" /etc/systemd/system/muteq-watchdog.service
  sudo cp -f "$SCRIPT_DIR/tools/muteq-watchdog.timer" /etc/systemd/system/muteq-watchdog.timer
  sudo systemctl daemon-reload
  sudo systemctl enable muteq-watchdog.timer
  sudo systemctl restart muteq-watchdog.timer
  echo "   Watchdog timer enabled (checks every 5 minutes)."
fi

echo ""
echo "✅ MUTEq C++ sensor service successfully installed and started."
echo "   Status:  sudo systemctl status $SERVICE_NAME"
echo "   Logs:    sudo journalctl -u $SERVICE_NAME -f"
echo "   Binary:  $TARGET_BIN"
if [ -n "$CONFIG_PATH" ]; then
  echo "   Config:  $CONFIG_PATH"
fi
