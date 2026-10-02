# syntax=docker/dockerfile:1
# Multi-stage Docker build for MUTEq Dashboard Server on Amazon Linux 2023 / EC2
# Can build an operational container image or export the compiled Linux binary directly.

ARG TARGETPLATFORM=linux/amd64

# Stage 1: Build environment using Amazon Linux 2023
FROM public.ecr.aws/amazonlinux/amazonlinux:2023 AS build

# Install GCC-14 (with full C++23 support), SQLite3 dev, OpenSSL dev, Make
RUN dnf install -y \
    gcc14-c++ \
    make \
    sqlite-devel \
    openssl-devel \
    tar \
    gzip && \
    dnf clean all

WORKDIR /app
COPY src/ ./src/
COPY static/ ./static/
COPY Makefile .

# Build optimized release binary with GCC 14
RUN CXX=gcc14-g++ make release && strip bin/muteq_server

# Stage 2: Minimal runtime container for EC2 deployment
FROM public.ecr.aws/amazonlinux/amazonlinux:2023-minimal AS runner

# Install only minimal runtime dependencies
RUN dnf install -y \
    sqlite-libs \
    openssl-libs \
    libstdc++ && \
    dnf clean all

WORKDIR /app

# Copy binary and web static assets
COPY --from=build /app/bin/muteq_server /app/muteq_server
COPY --from=build /app/static /app/static

# Default environment configuration
ENV MUTEQ_PORT=8080
ENV MUTEQ_HOST=0.0.0.0
ENV MUTEQ_DB=/data/muteq.db
ENV MUTEQ_STATIC_DIR=/app/static

# Persistent database storage mount point
VOLUME ["/data"]

EXPOSE 8080

ENTRYPOINT ["/app/muteq_server"]

# Stage 3: Export stage for extracting standalone Linux binary to host
FROM scratch AS export
COPY --from=build /app/bin/muteq_server /
