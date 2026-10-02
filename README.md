# MUTEq (C++23 Native Architecture)

<p align="center">
  <strong>High-Performance, Low-Latency Acoustic Intelligence & Telemetry Platform</strong><br>
  Native C++23 Sensor Daemon · Micro-Footprint Ingestion Engine · Modern Web Analytics
</p>

<p align="center">
  <a href="#-architecture--data-flow">Architecture</a> •
  <a href="#-why-native-c23">Why C++23?</a> •
  <a href="#-quick-start">Quick Start</a> •
  <a href="#-subsystems">Subsystems</a> •
  <a href="#-deployment">Deployment</a>
</p>

---

## 🎯 What is MUTEq?

**MUTEq** is a self-hostable, end-to-end acoustic monitoring platform designed to capture, process, and analyze ambient sound pressure levels (SPL) in real time.

Built to run headless on low-power edge nodes (e.g. Raspberry Pi) and cloud servers (AWS EC2), MUTEq continuously reads calibrated sound levels from a USB meter, performs digital signal smoothing, tracks threshold-breach events, buffers locally in SQLite using WAL mode, and streams telemetry securely to a central dashboard.

### Key Capabilities

- ⚡ **Ultra-Low Resource Footprint** — Native binaries running in **< 15 MB RSS** with sub-millisecond execution times. No Python runtime, garbage collection stalls, or virtual environment overhead.
- 🎯 **Deterministic Serial Processing** — High-speed POSIX `termios` raw serial parser with automatic framing resynchronization for CH340 sound meters at 115200 baud.
- 🛡️ **Zero-Loss Ring Buffer** — Lock-free circular queues decouple hardware I/O from storage and network transmission.
- 🗄️ **Flash-Optimized Storage** — Direct SQLite3 C API integration running in WAL mode with transactional batching to eliminate SD card flash memory wear.
- 🌐 **Instant Web Dashboard** — Multi-threaded HTTP server (`cpp-httplib` + `nlohmann/json`) serving responsive time-series charts, 5 dB histogram distributions, and 7-day hourly heatmaps with 100% backward compatibility.
- 🔐 **HMAC-SHA256 Ingestion** — Cryptographically signed payloads (`CommonCrypto` on macOS, `OpenSSL` on Linux) safeguarding ingestion endpoints.
- 🧪 **Offline Simulator** — Built-in pseudo-terminal (PTY) mock hardware meter allowing full end-to-end pipeline testing without physical USB hardware.

---

## ⚡ Architecture & Data Flow

```
Edge Sensor (Raspberry Pi / macOS)
┌─────────────────────────────────┐
│     USB SPL Hardware Meter      │
│   (CH340 framing @ 115200 baud) │
└────────────────┬────────────────┘
                 │ 6-byte frames: 0x55 <hi> <lo> 0x01 0x01 0xAA
                 ▼
┌─────────────────────────────────┐
│    Serial Reader Thread         │ ◀── POSIX termios / raw mode
│    (std::span frame validation) │
└────────────────┬────────────────┘
                 │ lock-free push
                 ▼
┌─────────────────────────────────┐
│    circular_buffer Queue        │ ◀── Decoupled ring buffer
└────────────────┬────────────────┘
                 │ pop
                 ▼
┌─────────────────────────────────┐
│    Consumer Worker Thread       │
├─────────────────────────────────┤
│ • Asymmetric EMA Filter         │ ◀── Fast attack (0.5), slow decay (0.05)
│ • Local SQLite (WAL Mode)       │ ◀── Batch writes, transaction buffering
│ • Immediate & Periodic Flush    │ ◀── Event-driven dispatch + 15s timer
└────────────────┬────────────────┘
                 │ POST /api/ingest (HMAC-SHA256)
                 ▼
Cloud Server (AWS EC2 / Linux / Container)
┌─────────────────────────────────┐
│    MUTEq Dashboard Server       │
├─────────────────────────────────┤
│ • Multi-Threaded HTTP/1.1 Engine│ ◀── 12-worker thread pool
│ • Central SQLite Repository     │ ◀── Aggregations, heatmaps, percentiles
│ • Responsive Web UI (static/)   │ ◀── TradingView Lightweight Charts
└─────────────────────────────────┘
```

---

## 🚀 Why Native C++23?

| Dimension | Legacy Python Implementation | Native C++23 Implementation |
|---|---|---|
| **Memory (RSS)** | ~120 MB to 180 MB RSS | **< 15 MB RSS** |
| **Serial I/O** | `pySerial` / `pyusb` with GC jitter | **POSIX termios raw mode** with deterministic packet parsing |
| **Thread Decoupling** | Python GIL (Global Interpreter Lock) contention | **Zero-lock circular queue** (`circular_buffer`) |
| **Edge Storage** | Python `sqlite3` ORM overhead | **Direct SQLite C API** with WAL mode & explicit transactions |
| **Web Server** | FastAPI + Uvicorn + Starlette + AnyIO | **Single-header multi-threaded `cpp-httplib`** |
| **Deployment** | Python 3.11+, `venv`, `pip`, heavy dependencies | **Single standalone ELF binary** (zero external dependencies) |
| **Startup Time** | 2.5 – 5.0 seconds | **< 15 milliseconds** |

---

## 📂 Subsystems

The project is structured into two focused, self-contained native components:

```
muteq_in_c/
├── muteq_sensor/               # Edge sensor daemon & hardware interfaces
│   ├── src/
│   │   ├── circular_buffer.hpp # Lock-free ring buffer
│   │   ├── http_egress.hpp     # libcurl HTTP client + HMAC signing
│   │   ├── main.cpp            # Multi-threaded pipeline coordinator
│   │   ├── smoothing.hpp       # Asymmetric EMA digital filter
│   │   ├── spl_db.hpp          # SQLite domain storage manager
│   │   ├── spl_reader.hpp      # CH340 serial framing and parser
│   │   └── sqlite_storage.hpp  # Low-level SQLite3 C API RAII wrapper
│   ├── tools/
│   │   └── mock_spl_meter.cpp  # PTY hardware simulator
│   ├── Dockerfile              # Multi-stage Linux ARM64 cross-compiler
│   └── Makefile                # Targets: build, mock, cross-pi, clean
│
└── muteq_server/               # Dashboard server & telemetry ingestion engine
    ├── src/
    │   ├── auth.hpp            # HMAC-SHA256 signature verification
    │   ├── db.hpp              # SQLite analytics engine (heatmaps, percentiles)
    │   ├── httplib.h           # Single-header HTTP/1.1 multi-threaded server
    │   ├── json.hpp            # Single-header JSON serializer/parser
    │   └── main.cpp            # Server endpoints & static file router
    ├── static/                 # Frontend dashboard web assets
    ├── Dockerfile              # Multi-stage Amazon Linux 2023 build
    └── Makefile                # Targets: build, docker-build, export-bin
```

---

## 🚀 Quick Start

### Prerequisites

1. **Supported USB Sound Meter**:
   A calibrated USB SPL meter using CH340 serial communication (`115200 baud`, `0x1a86:0x7523` or compatible).
   
   → **Search for "volume meter HY1361" on AliExpress — this model is tested and will be detected automatically:**

   <p align="center">
     <img src="https://github.com/user-attachments/assets/3148ad40-651c-4b55-9bf0-4b42e53e2c46" width="45%" alt="HY1361 USB Sound Level Meter on AliExpress" />
     <br/>
     <em>Standard HY1361 USB Sound Level Meter (CH340 chipset)</em>
   </p>

2. **Edge Hardware / Host**:
   - Raspberry Pi 3/4/5 (Raspberry Pi OS 64-bit) or any Linux / macOS computer.
   - *(Optional)* No hardware required for testing — use the built-in `mock_spl_meter` to simulate live data via a pseudo-terminal PTY!

3. **Software Toolchain**:
   - C++23 compliant compiler (`clang++` >= 16 or `g++` >= 13)
   - Development libraries: `sqlite3`, `libcurl`, `pthread`, OpenSSL (`libssl-dev` / `openssl-devel` on Linux)
   - Docker BuildKit (optional, for cross-compiling or container deployment)

---

### 1. Build Both Binaries

Both components use standard Makefiles compiling with `-std=c++23 -O3 -DNDEBUG`:

```bash
# Build Dashboard Server
cd muteq_server && make
cd ..

# Build Sensor Daemon & Mock Hardware Meter
cd muteq_sensor && make && make mock-meter
cd ..
```

---

### 2. Local Real-Time Testing (No Hardware Required)

You can validate the entire pipeline locally without physical hardware:

**Terminal 1 — Start the Dashboard Server:**
```bash
cd muteq_server
./bin/muteq_server
```
*Server starts on `http://0.0.0.0:8080` using `/private/tmp/muteq_server.db`.*

**Terminal 2 — Start the Mock SPL Hardware Simulator:**
```bash
cd muteq_sensor
make mock
```
*Creates a virtual serial PTY symlinked to `/tmp/ttyUSB0` transmitting calibrated CH340 frames.*

**Terminal 3 — Start the Sensor Client:**
```bash
cd muteq_sensor
./bin/muteq_sensor
```
*Automatically connects to `/tmp/ttyUSB0`, buffers readings into `/private/tmp/muteq.db`, and flushes events to `http://127.0.0.1:8080/api/ingest`.*

Open **[http://localhost:8080](http://localhost:8080)** in your browser to watch live telemetry stream in real time.

---

## ☁️ Deployment

### Edge Sensor: Raspberry Pi (Linux ARM64)

Cross-compile a native Linux ARM64 ELF binary on your development machine using Docker BuildKit (no local ARM cross-compiler required):

```bash
cd muteq_sensor
make cross-pi
# Outputs: bin/linux_arm64/muteq_sensor
```

Copy the binary to your Raspberry Pi and run it as a systemd service:

```bash
scp bin/linux_arm64/muteq_sensor pi@raspberrypi.local:/usr/local/bin/
```

### Dashboard Server: AWS EC2 (Amazon Linux 2023)

Build and deploy either as a standalone native binary via systemd (production) or as a minimal Docker container:

```bash
cd muteq_server

# 1. Export standalone Linux ARM64 (AWS Graviton) binary via macOS container or Docker:
make export-bin-arm64
# Outputs: bin/linux_arm64/muteq_server

# (Or for standard x86_64 EC2 instances):
make export-bin
# Outputs: bin/linux_x86_64/muteq_server

# 2. Deploy or update the CloudFormation infrastructure (EC2, Nginx, Let's Encrypt DNS-01, EIP):
make aws-deploy HostedZoneId=ZXXX KeyPairName=my-key HmacSecret=... AcmeEmail=you@example.com

# 3. Check CloudFormation stack status:
make aws-status

# 4. Check & Renew Let's Encrypt SSL certificates remotely:
make aws-ssl-status
make aws-ssl-renew
```

The production server runs directly as a native systemd unit (`muteq-dashboard.service`) at `/opt/muteq_server/bin/muteq_server` with static assets at `/opt/muteq_server/static` and SQLite database at `/var/lib/muteq-dashboard/muteq.db`, consuming **< 75 MB RSS** (compared to 226+ MB with Python).

---

## 🔒 Privacy & Architecture Guarantees

- **No Audio Captured** — Only scalar sound pressure level (dB) measurements are sampled. No microphone audio streams are ever digitized or recorded.
- **Local-First Storage** — All raw telemetry is saved to local SQLite first. Network egress drains and marks records sent asynchronously, allowing weeks of offline operation without data loss.
- **Minimal Wear on Flash Media** — WAL journal mode combined with batch transactions prevents SD card corruption on edge nodes.
