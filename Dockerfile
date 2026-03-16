# ============================================================================
# Multi-stage Dockerfile for tv-webhook-trader
# ============================================================================

# -- Stage 1: Builder --
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libboost-all-dev \
    libssl-dev \
    libspdlog-dev \
    libfmt-dev \
    nlohmann-json3-dev \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt .
COPY src/ src/
COPY include/ include/
COPY tests/ tests/
COPY config/ config/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
    && cmake --build build -j$(nproc)

# -- Stage 2: Runtime --
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libboost-system1.74.0 \
    libssl3 \
    libspdlog1 \
    libfmt8 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/tv_webhook_trader /app/tv_webhook_trader
COPY --from=builder /src/config/ /app/config/
COPY static/ /app/static/

# Webhook port + Dashboard port
EXPOSE 8443 5000

ENTRYPOINT ["/app/tv_webhook_trader"]
