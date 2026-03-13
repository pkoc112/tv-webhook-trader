#!/bin/bash
# ============================================================================
# build.sh — 빌드 스크립트
# v1.0 | 2026-03-13
# ============================================================================
set -e

BUILD_TYPE=${1:-Release}
BUILD_DIR="build"

echo "=== Building tv_webhook_trader (${BUILD_TYPE}) ==="

# 의존성 확인
for pkg in cmake g++ libboost-all-dev libssl-dev libfmt-dev libspdlog-dev; do
    if ! dpkg -l | grep -q "$pkg"; then
        echo "Installing $pkg..."
        sudo apt-get install -y "$pkg"
    fi
done

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build . -j$(nproc)

echo "=== Build complete: ${BUILD_DIR}/tv_webhook_trader ==="
