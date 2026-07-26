#!/bin/bash
# Rebuild, install, and restart power-balance service.
# Usage: ./rebuild.sh [--test]
#   --test  also run unit tests before installing

set -euo pipefail

cd "$(dirname "$0")"

echo "▶ Stopping power-balance.service..."
sudo systemctl stop power-balance.service

echo "▶ Building..."
mkdir -p build && cd build
make -j"$(nproc)"

if [[ "${1:-}" == "--test" ]]; then
    echo "▶ Running tests..."
    ctest --output-on-failure
fi

echo "▶ Installing..."
sudo make install

echo "▶ Starting power-balance.service..."
sudo systemctl start power-balance.service
sleep 1

echo "▶ Status:"
sudo systemctl status power-balance.service --no-pager --lines 5

sleep 1
sudo ./power-status
