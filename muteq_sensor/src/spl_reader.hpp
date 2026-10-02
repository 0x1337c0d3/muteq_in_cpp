//
//  spl_reader.hpp
//  muteq_sensor
//
//  POSIX serial/TTY reader for sound pressure level (SPL) meters.
//  Implements the canonical CH340 6-byte packet framing matching usb_reader.py.
//

#pragma once

#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace muteq {

struct SplReading {
    float db_a;
    uint8_t raw_high;
    uint8_t raw_low;
};

class SplMeterReader {
  public:
    static constexpr uint8_t SERIAL_START = 0x55;
    static constexpr uint8_t SERIAL_END = 0xAA;
    static constexpr size_t SERIAL_PKT_LEN = 6;
    static constexpr float SPL_MIN = 30.0f;
    static constexpr float SPL_MAX = 130.0f;

    explicit SplMeterReader(std::string_view port_name) {
        const std::string port_str(port_name);
        fd_ = ::open(port_str.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to open serial port " + port_str);
        }

        // Configure 115200 8N1 raw mode
        termios tty{};
        if (::tcgetattr(fd_, &tty) != 0) {
            ::close(fd_);
            throw std::system_error(errno, std::generic_category(), "Failed to get termios attr");
        }

        ::cfsetspeed(&tty, B115200);
        tty.c_cflag |= (CS8 | CREAD | CLOCAL);
        tty.c_cflag &= ~static_cast<tcflag_t>(PARENB | CSTOPB | CRTSCTS);
        tty.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY | ICRNL | INLCR);
        tty.c_oflag &= ~static_cast<tcflag_t>(OPOST);

        // Non-blocking reads handled via small poll/yield intervals
        tty.c_cc[VTIME] = 1; // 100ms
        tty.c_cc[VMIN] = 0;

        ::tcflush(fd_, TCIFLUSH);
        ::tcsetattr(fd_, TCSANOW, &tty);
    }

    ~SplMeterReader() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    // Disable copies, allow moves
    SplMeterReader(const SplMeterReader&) = delete;
    SplMeterReader& operator=(const SplMeterReader&) = delete;

    SplMeterReader(SplMeterReader&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), rx_buf_(std::move(other.rx_buf_)) {
    }

    SplMeterReader& operator=(SplMeterReader&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
            rx_buf_ = std::move(other.rx_buf_);
        }
        return *this;
    }

    /// Decode canonical 6-byte CH340 framed packet:
    ///   0x55  [b_high]  [b_low]  0x01  0x01  0xAA
    ///   dB = ((b_high << 8) | b_low) / 10.0
    static constexpr std::optional<SplReading> parse_ch340_frame(std::span<const uint8_t> frame) noexcept {
        if (frame.size() < SERIAL_PKT_LEN) {
            return std::nullopt;
        }
        if (frame[0] != SERIAL_START || frame[SERIAL_PKT_LEN - 1] != SERIAL_END) {
            return std::nullopt;
        }

        const uint8_t high = frame[1];
        const uint8_t low = frame[2];
        const float db = static_cast<float>((static_cast<uint16_t>(high) << 8) | low) / 10.0f;

        if (db < SPL_MIN || db > SPL_MAX) {
            return std::nullopt;
        }

        return SplReading{.db_a = db, .raw_high = high, .raw_low = low};
    }

    /// Read next valid sample from the serial descriptor, with automatic stream re-sync.
    std::optional<SplReading> read_next_sample() noexcept {
        uint8_t buffer[64];
        ssize_t n = ::read(fd_, buffer, sizeof(buffer));
        if (n > 0) {
            rx_buf_.insert(rx_buf_.end(), buffer, buffer + n);
        } else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return std::nullopt; // Error on descriptor
        }

        // Search for 6-byte packet: 0x55 hi lo 0x01 0x01 0xAA
        while (true) {
            auto start_it = std::find(rx_buf_.begin(), rx_buf_.end(), SERIAL_START);
            if (start_it == rx_buf_.end()) {
                rx_buf_.clear();
                break;
            }
            if (start_it != rx_buf_.begin()) {
                rx_buf_.erase(rx_buf_.begin(), start_it); // Discard leading garbage
            }
            if (rx_buf_.size() < SERIAL_PKT_LEN) {
                break; // Wait for full packet to arrive
            }
            if (rx_buf_[SERIAL_PKT_LEN - 1] == SERIAL_END) {
                auto parsed = parse_ch340_frame(std::span<const uint8_t>(rx_buf_.data(), SERIAL_PKT_LEN));
                rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + SERIAL_PKT_LEN);
                if (parsed.has_value()) {
                    return parsed;
                }
            } else {
                // False start byte; drop it and continue scanning
                rx_buf_.erase(rx_buf_.begin());
            }
        }

        return std::nullopt;
    }

  private:
    int fd_{-1};
    std::vector<uint8_t> rx_buf_;
};

} // namespace muteq
