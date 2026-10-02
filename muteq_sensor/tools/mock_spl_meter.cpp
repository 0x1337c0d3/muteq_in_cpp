//
//  mock_spl_meter.cpp
//  muteq_sensor
//
//  Mock hardware SPL meter emitting CH340 6-byte binary packets over a pseudo-terminal (PTY).
//  Symlinks the slave PTY to /tmp/ttyUSB0 for easy testing without hardware.
//

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <termios.h>
#include <thread>
#include <unistd.h>

#if defined(__APPLE__)
#    include <util.h>
#else
#    include <pty.h>
#endif

namespace {

constexpr std::string_view SYMLINK_PATH = "/tmp/ttyUSB0";
std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

void cleanup_symlink() {
    ::unlink(SYMLINK_PATH.data());
}

} // namespace

int main(int argc, char* argv[]) {
    // Optional symlink destination from command line
    std::string link_target = (argc > 1) ? argv[1] : std::string(SYMLINK_PATH);

    int master_fd = -1;
    int slave_fd = -1;
    char slave_name[256]{};

    if (::openpty(&master_fd, &slave_fd, slave_name, nullptr, nullptr) < 0) {
        std::cerr << "Failed to allocate pseudo-terminal (openpty): " << std::strerror(errno) << "\n";
        return 1;
    }

    // Configure slave PTY in raw mode
    termios tty{};
    if (::tcgetattr(slave_fd, &tty) == 0) {
        ::cfmakeraw(&tty);
        ::cfsetspeed(&tty, B115200);
        ::tcsetattr(slave_fd, TCSANOW, &tty);
    }

    // Create or update symlink
    ::unlink(link_target.c_str());
    if (::symlink(slave_name, link_target.c_str()) != 0) {
        std::cerr << std::format("Warning: Could not create symlink '{}' -> '{}': {}\n", link_target, slave_name,
                                 std::strerror(errno));
    } else {
        std::cout << std::format("[MOCK] Symlinked '{}' -> '{}'\n", link_target, slave_name);
    }

    // Handle Ctrl+C and termination signals
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::atexit(cleanup_symlink);

    std::cout << "====================================================\n";
    std::cout << " MUTEq SPL Hardware Mock Server\n";
    std::cout << std::format(" Port Device: {}\n", slave_name);
    std::cout << std::format(" Test Target: {}\n", link_target);
    std::cout << " Protocol:    CH340 6-byte frames at 115200 baud\n";
    std::cout << " Frame:       0x55 [b_high] [b_low] 0x01 0x01 0xAA (every 500ms)\n";
    std::cout << " Rate:        2 Hz (every 500 ms)\n";
    std::cout << " Press Ctrl+C to stop mock device\n";
    std::cout << "====================================================\n\n";

    std::mt19937 rng(std::random_device{}());
    std::normal_distribution<float> noise_dist(0.0f, 1.2f);
    std::uniform_real_distribution<float> spike_prob(0.0f, 1.0f);

    float base_level = 52.0f;
    int tick = 0;

    while (g_running) {
        // Generate realistic sound level: subtle drift + noise
        float db = base_level + 3.0f * std::sin(static_cast<float>(tick) * 0.1f) + noise_dist(rng);

        // Inject occasional noise spike >= 70 dBA (threshold breach) every ~15 ticks (7.5s)
        if (tick > 0 && (tick % 15 == 0 || spike_prob(rng) < 0.08f)) {
            db = 74.0f + std::abs(noise_dist(rng)) * 3.5f;
            std::cout << " >>> INJECTING THRESHOLD EVENT: ";
        }

        db = std::clamp(db, 35.0f, 120.0f);

        // Encode 6-byte packet: dB = int.from_bytes([high, low]) / 10.0
        const auto raw = static_cast<uint16_t>(std::round(db * 10.0f));
        const uint8_t high = static_cast<uint8_t>((raw >> 8) & 0xFF);
        const uint8_t low = static_cast<uint8_t>(raw & 0xFF);

        const uint8_t packet[6] = {
            0x55, // Start marker
            high, // MSB
            low,  // LSB
            0x01, // Mode
            0x01, // Weighting / Fast
            0xAA  // End marker
        };

        ssize_t written = ::write(master_fd, packet, sizeof(packet));
        if (written == sizeof(packet)) {
            std::cout << std::format("[{:04d}] Emitted: {:.1f} dBA (raw: 0x{:02X} 0x{:02X})\n", tick, db, high, low);
        } else if (written < 0 && errno != EAGAIN) {
            std::cerr << "Write error to master PTY: " << std::strerror(errno) << "\n";
            break;
        }

        ++tick;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\nStopping mock device and cleaning up...\n";
    cleanup_symlink();
    ::close(master_fd);
    ::close(slave_fd);

    return 0;
}
