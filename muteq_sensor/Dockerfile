# syntax=docker/dockerfile:1
# Multi-stage Docker build targeting Raspberry Pi (Linux ARM64 / aarch64)
# Exports the compiled ELF binary directly to the host filesystem.

FROM --platform=linux/arm64 debian:bookworm-slim AS build

# Install Clang 19 (supporting full C++23 std::println/format) and target libraries directly on Debian 12 (Bookworm)
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang-19 \
    libc++-19-dev \
    libc++abi-19-dev \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY src/ ./src/

# Compile optimized release C++23 binary matching Raspberry Pi OS glibc (2.36)
RUN mkdir -p /out && \
    clang++-19 -std=c++23 -stdlib=libc++ -D_LIBCPP_ENABLE_EXPERIMENTAL -static-libstdc++ \
    -O3 -DNDEBUG -Wall -Wextra -Wpedantic \
    -Isrc \
    src/main.cpp \
    -o /out/muteq_sensor \
    -lsqlite3 -lpthread -lcurl -lssl -lcrypto && \
    strip /out/muteq_sensor

# Minimal export stage to output only the compiled ELF binary to host
FROM scratch AS export
COPY --from=build /out/muteq_sensor /
