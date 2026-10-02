# MUTEq Sensor (`muteq_sensor`)

High-performance, deterministic sound level monitoring daemon written in **modern C++23**. Designed for embedded Linux (Raspberry Pi OS) and macOS development environments.

Replaces the legacy Python telemetry pipeline (`mute/sensor/app/`) with a native binary running in **< 15 MB RSS**, featuring zero garbage collection latency, hardware serial resynchronization, lock-free ring buffering, direct SQLite C API storage, and HTTP egress to the central dashboard.

---

## Architecture Overview

```
                      ┌───────────────────────────────┐
                      │    Hardware SPL Meter         │
                      │  CH340 / POSIX termios 115200 │
                      └───────────────┬───────────────┘
                                      │ 6-byte frames
                                      ▼
                      ┌───────────────────────────────┐
                      │    Serial Reader Thread       │
                      │  std::span packet validation  │
                      └───────────────┬───────────────┘
                                      │ push (samples & events)
                                      ▼
                      ┌───────────────────────────────┐
                      │   circular_buffer Ring Queue  │
                      │     (lock-free / decoupled)   │
                      └───────────────┬───────────────┘
                                      │ pop
                                      ▼
                      ┌───────────────────────────────┐
                      │   Consumer / Storage Worker   │
                      ├───────────────────────────────┤
                      │  • Asymmetric EMA Smoothing   │
                      │  • SQLite C API (WAL mode)    │
                      │  • Immediate & Periodic Flush │
                      └───────────────┬───────────────┘
                                      │ POST /api/ingest
                                      ▼
                      ┌───────────────────────────────┐
                      │    MUTEq Dashboard Server     │
                      │      (libcurl + HMAC-SHA256)  │
                      └───────────────────────────────┘
```

### Core Components

- **Serial Reader ([`src/spl_reader.hpp`](file:///Users/peter/Development/muteq_in_c/muteq_sensor/src/spl_reader.hpp))**:
  Reads raw serial bytes over POSIX `termios` at 115200 baud. Decodes canonical 6-byte CH340 framing (`0x55 <high> <low> 0x01 0x01 0xAA`), computes `dB = ((high << 8) | low) / 10.0`, validates `30.0 <= dB <= 130.0`, and automatically resynchronizes on corrupt packets.
- **Ring Buffer ([`src/circular_buffer.hpp`](file:///Users/peter/Development/muteq_in_c/muteq_sensor/src/circular_buffer.hpp))**:
  Thread-safe circular queue decoupling the high-frequency serial reader thread from disk I/O and network latency.
- **Digital Smoothing ([`src/smoothing.hpp`](file:///Users/peter/Development/muteq_in_c/muteq_sensor/src/smoothing.hpp))**:
  `constexpr AsymmetricEMA` filter (alpha_rise = 0.5, alpha_fall = 0.05) matching sound meter fast-attack / slow-decay behavior.
- **Direct SQLite Storage ([`src/sqlite_storage.hpp`](file:///Users/peter/Development/muteq_in_c/muteq_sensor/src/sqlite_storage.hpp) & [`src/spl_db.hpp`](file:///Users/peter/Development/muteq_in_c/muteq_sensor/src/spl_db.hpp))**:
  Direct SQLite3 C API wrapper running with `PRAGMA journal_mode = WAL;` and `PRAGMA synchronous = NORMAL;`. Eliminates SD card wear via transaction batching. Stores `readings` (every cycle) and `events` (when >= 70.0 dBA).
- **HTTP Egress Client ([`src/http_egress.hpp`](file:///Users/peter/Development/muteq_in_c/muteq_sensor/src/http_egress.hpp))**:
  `libcurl` client that batches unsent local SQLite records, signs payloads with HMAC-SHA256 (`CommonCrypto` on macOS, `OpenSSL` on Linux), and streams them to `/api/ingest`. Fires **immediately** upon threshold breach events and **periodically** (every 15s) for background baseline readings.

---

## Directory Layout

```
muteq_sensor/
├── src/
│   ├── circular_buffer.hpp     # Thread-safe ring buffer
│   ├── http_egress.hpp         # libcurl client & HMAC-SHA256 signer
│   ├── main.cpp                # Multi-threaded pipeline coordinator
│   ├── smoothing.hpp           # Asymmetric EMA filter
│   ├── spl_db.hpp              # Domain-level SQLite manager
│   ├── spl_reader.hpp          # POSIX serial port framing & decoder
│   └── sqlite_storage.hpp      # Low-level SQLite3 RAII wrapper
├── tools/
│   └── mock_spl_meter.cpp      # Pseudo-terminal (PTY) hardware simulator
├── Dockerfile                  # Multi-stage Linux ARM64 (Raspberry Pi) cross-compiler
├── CMakeLists.txt              # Standard CMake build configuration
├── Makefile                    # Developer build targets & formatting tools
└── README.md
```

---

## Building & Running

### Hardware Requirements
- **Supported USB Sound Meter**:
  A calibrated USB SPL meter using CH340 serial communication (`115200 baud`, `0x1a86:0x7523` or compatible).
  
  → **Search for "volume meter HY1361" on AliExpress — this model is tested and will be detected automatically:**

  <p align="center">
    <img src="https://github.com/user-attachments/assets/3148ad40-651c-4b55-9bf0-4b42e53e2c46" width="45%" alt="HY1361 USB Sound Level Meter on AliExpress" />
    <br/>
    <em>Standard HY1361 USB Sound Level Meter (CH340 chipset)</em>
  </p>

### Software Requirements
- **macOS / Linux**: C++23 compliant compiler (`clang++` >= 16 or `g++` >= 13)
- **Libraries**: `sqlite3`, `libcurl`, `pthread`, and OpenSSL (Linux only)

### Available `make` Targets

| Target | Description |
|---|---|
| `make build` | Builds the native release binary (`bin/muteq_sensor`) |
| `make debug` | Builds with debug symbols (`-g -O0`) |
| `make release` | Builds optimized release binary (`-O3 -DNDEBUG`) |
| `make sanitize` | Builds with Address and Undefined Behavior Sanitizers (`-fsanitize=address,undefined`) |
| `make mock-meter` | Compiles the mock hardware PTY simulator (`bin/mock_spl_meter`) |
| `make mock` | Compiles and executes the mock hardware meter |
| `make cross-pi` | Builds a standalone Linux ARM64 ELF binary for Raspberry Pi via Docker |
| `make format` | Formats all source files in-place using `.clang-format` |
| `make format-check`| Verifies formatting without modifying files |
| `make clean` | Removes `build/` and `bin/` directories |
| `make run` | Builds and runs the binary |
| `make aws-deploy` | Deploy (or update) the EC2 dashboard CloudFormation stack |
| `make aws-status` | Show current CloudFormation stack status and outputs |
| `make aws-delete` | Tear down the CloudFormation stack |
| `make aws-ssl-renew` | Renew Let's Encrypt SSL certificate on EC2 (FORCE=1 to force) |
| `make aws-ssl-status`| Show Let's Encrypt SSL certificate expiry and details on EC2 |

---

## Running with the Mock SPL Meter (No Hardware Needed)

You can run and test the complete pipeline in real time on macOS without physical USB hardware connected:

1. **Terminal 1: Start the Dashboard Server**
   ```bash
   cd ../muteq_server
   make build && ./bin/muteq_server
   ```

2. **Terminal 2: Start the Mock Hardware Simulator**
   ```bash
   cd ../muteq_sensor
   make mock
   ```
   *Creates a PTY slave symlinked to `/tmp/ttyUSB0` and transmits synthetic sound packets with random noise peaks.*

3. **Terminal 3: Start the Sensor Daemon**
   ```bash
   cd ../muteq_sensor
   make run
   ```
   *Automatically connects to `/tmp/ttyUSB0`, buffers readings in `/private/tmp/muteq.db`, and flushes events to `http://localhost:8080`.*

---

## Configuration via Environment Variables

All settings can be configured via environment variables:

| Variable | Default | Purpose |
|---|---|---|
| `MUTEQ_SERIAL_PORT` | `/dev/ttyUSB0` (or `/tmp/ttyUSB0` if mock exists) | Serial device path for sound meter |
| `MUTEQ_SERVER_URL` | `http://127.0.0.1:8080` | Destination dashboard server URL |
| `MUTEQ_HMAC_SECRET` | *(empty / open)* | Optional secret key for HMAC-SHA256 signature verification |
| `MUTEQ_DEVICE_ID` | `muteq-sensor-01` | Unique sensor device identifier |

---

## Cross-Compiling for Raspberry Pi (Linux ARM64)

Build a clean Linux ARM64 ELF binary without needing an ARM toolchain installed on your Mac:

```bash
make cross-pi
```

Outputs the stripped release ELF binary directly to `bin/linux_arm64/muteq_sensor`. Copy this binary directly to your Raspberry Pi:

```bash
scp bin/linux_arm64/muteq_sensor pi@raspberrypi.local:/usr/local/bin/
```
