#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
make all tests
./tests

if [[ "${1:-}" != "--model-only" ]]; then
    ./test_network.sh
fi
