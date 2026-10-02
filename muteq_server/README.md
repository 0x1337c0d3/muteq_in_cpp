# MUTEq Dashboard Server (`muteq_server`)

High-performance, ultra-lean web dashboard and telemetry ingestion server written in **modern C++23**. Designed for AWS EC2 (Amazon Linux 2023 / systemd / Docker) and local macOS development.

Replaces the legacy Python FastAPI/Uvicorn server (`mute/dashboard/server.py`) with a native binary that starts instantly, uses **< 15 MB RSS**, serves sub-millisecond API responses, and maintains **100% backward compatibility** with the existing frontend web dashboard.

---

## Features

- **High-Throughput Ingestion (`POST /api/ingest`)**:
  Receives batch readings and threshold events from remote sensors. Executes atomic SQLite transactions with WAL mode and optional HMAC-SHA256 signature verification.
- **Identical REST API**:
  Serves all JSON endpoints queried by [`static/dashboard.js`](file:///Users/peter/Development/muteq_in_c/muteq_server/static/dashboard.js) with zero modifications required on the frontend.
- **Embedded Web Server**:
  Powered by `cpp-httplib` using a 12-worker thread pool to serve static HTML, CSS, and JS assets without requiring Nginx or an external reverse proxy.
- **Zero Heavy Dependencies**:
  Built with single-header libraries (`cpp-httplib`, `nlohmann/json`) and direct SQLite3 / OpenSSL C APIs. No Python runtime, no virtual environment, and no package manager needed on production servers.

---

## Directory Layout

```
muteq_server/
├── src/
│   ├── auth.hpp        # HMAC-SHA256 verification (CommonCrypto on macOS / OpenSSL on Linux)
│   ├── db.hpp          # Thread-safe SQLite backend (aggregations, histograms, heatmap, percentiles)
│   ├── httplib.h       # Single-header HTTP/1.1 multi-threaded web server
│   ├── json.hpp        # Single-header JSON parser and serializer
│   └── main.cpp        # Server setup, CLI/env routing, and HTTP endpoint handlers
├── static/
│   ├── index.html      # Responsive web dashboard UI
│   ├── dashboard.css   # Stylesheet
│   └── dashboard.js    # Live telemetry charting, KPIs, and heatmap rendering
├── Dockerfile          # Multi-stage Amazon Linux 2023 build (runtime image & ELF export)
├── Makefile            # Developer targets, Docker cross-compilation, and release builds
└── README.md
```

---

## API Reference

| Method | Path | Description |
|---|---|---|
| `GET` | `/` | Serves `static/index.html` |
| `GET` | `/static/*` | Mounts and serves static web assets (`dashboard.css`, `dashboard.js`, etc.) |
| `GET` | `/api/config` | Returns device metadata (`device_name`, `location`, `environment_profile`) |
| `GET` | `/api/readings` | Downsampled time-series data: `[{time, value}]`. Accepts `from_ts`, `to_ts`, `limit`. |
| `GET` | `/api/events` | Threshold events: `[{time, noise, peak}]`. Accepts `from_ts`, `to_ts`, `limit`. |
| `GET` | `/api/stats` | KPI metrics (`latest`, `peak`, `event_count`), p50/p90/p99 percentiles, 5 dB histogram bins, 7-day hourly heatmap matrix, and 30-day daily breakdown. |
| `POST` | `/api/ingest` | Receives JSON `{ readings: [...], events: [...] }` with optional `X-HMAC-Signature: sha256=...`. |

---

## Building & Running Locally

### Requirements
- **macOS / Linux**: C++23 compiler (`clang++` >= 16 or `g++` >= 13)
- **Libraries**: `sqlite3`, `pthread`, and OpenSSL (`libssl-dev` / `openssl-devel` on Linux)

### Build Targets

```bash
# Build release binary (bin/muteq_server)
make build

# Build with debug symbols
make debug

# Run server locally on port 8080
make run

# Clean build artifacts
make clean

# Deploy or query CloudFormation stack
make aws-deploy HostedZoneId=Z... KeyPairName=my-key HmacSecret=... AcmeEmail=you@example.com
make aws-status

# Manage Let's Encrypt SSL on EC2
make aws-ssl-status
make aws-ssl-renew
```

Once running, navigate to [http://localhost:8080](http://localhost:8080) in your browser.

---

## Configuration via Environment Variables

All settings can be configured via environment variables:

| Variable | Default | Purpose |
|---|---|---|
| `MUTEQ_PORT` | `8080` | Port to bind the HTTP server to |
| `MUTEQ_HOST` | `0.0.0.0` | Host IP address to bind to |
| `MUTEQ_DB` | `/private/tmp/muteq_server.db` | Path to central SQLite database |
| `MUTEQ_STATIC_DIR` | Auto-detected (`static`) | Path to static frontend assets directory |
| `MUTEQ_HMAC_SECRET` | *(empty / open)* | Secret key for verifying `X-HMAC-Signature` headers on `/api/ingest` |

---

## Deploying to AWS EC2 (Amazon Linux 2023)

### Option 1: Standalone Linux ELF Binary via systemd (Production)

You can build stripped standalone Linux binaries directly on your Mac using `container` (macOS native) or Docker BuildKit with GCC 14 (`gcc14-c++`):

```bash
# AWS Graviton instances (ARM64 / aarch64, e.g. t4g.micro, c7g):
make export-bin-arm64
# Outputs: bin/linux_arm64/muteq_server

# Standard x86_64 EC2 instances (e.g. t3.micro, c6i):
make export-bin
# Outputs: bin/linux_x86_64/muteq_server
```

Deploy binary and static assets via systemd on EC2:
1. Copy the binary and static assets:
   ```bash
   sudo mkdir -p /opt/muteq_server/bin /opt/muteq_server/static
   sudo cp bin/linux_arm64/muteq_server /opt/muteq_server/bin/
   sudo cp -r static/* /opt/muteq_server/static/
   ```
2. Configure `/etc/muteq-dashboard.env`:
   ```ini
   MUTEQ_PORT=8080
   MUTEQ_HOST=127.0.0.1
   MUTEQ_DB=/var/lib/muteq-dashboard/muteq.db
   MUTEQ_STATIC_DIR=/opt/muteq_server/static
   MUTEQ_HMAC_SECRET=<shared-hmac-secret>
   ```
3. Install the systemd service in `/etc/systemd/system/muteq-dashboard.service`:
   ```ini
   [Unit]
   Description=MUTEq C++23 Dashboard Server
   After=network.target

   [Service]
   Type=simple
   WorkingDirectory=/opt/muteq_server
   EnvironmentFile=/etc/muteq-dashboard.env
   ExecStart=/opt/muteq_server/bin/muteq_server
   Restart=always
   RestartSec=5

   [Install]
   WantedBy=multi-user.target
   ```
4. Enable and start the service:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable --now muteq-dashboard
   ```

### Option 2: Docker Container

1. **Build the container image**:
   ```bash
   make docker-build
   ```

2. **Run container with persistent volume**:
   ```bash
   docker run -d \
     --name muteq-server \
     -p 8080:8080 \
     -e MUTEQ_HMAC_SECRET="your-secret-key" \
     -v /var/data/muteq:/data \
     --restart unless-stopped \
     muteq-server:latest
   ```

---

## CloudFormation & SSL Certificate Management

The stack deploys an EC2 instance with Nginx HTTPS reverse proxy, Route 53 DNS-01 Let's Encrypt certificate, and Elastic IP.

* **Deploy or Update Stack**:
  ```bash
  make aws-deploy HostedZoneId=ZXXX KeyPairName=my-key HmacSecret=... AcmeEmail=you@example.com
  ```
* **View Stack Status & Outputs**:
  ```bash
  make aws-status
  ```
* **Check SSL Certificate Status**:
  ```bash
  make aws-ssl-status
  ```
* **Renew SSL Certificate**:
  ```bash
  make aws-ssl-renew
  # Or force renewal immediately:
  make aws-ssl-renew FORCE=1
  ```
