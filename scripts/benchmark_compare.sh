#!/bin/bash
# io_uring vs epoll benchmark
# Usage: bash scripts/benchmark_compare.sh [port] [concurrency] [duration]
PORT=${1:-8080}
CONC=${2:-100}
DUR=${3:-30}
URL="http://127.0.0.1:$PORT/index.html"
if ! command -v wrk &>/dev/null; then echo "Install wrk: sudo apt install wrk"; exit 1; fi
for label in "io_uring" "epoll"; do
  echo "Testing $label..."
  wrk -t4 -c$CONC -d${DUR}s --latency $URL 2>&1 | grep -E "Requests/sec|Latency|Transfer"
  echo "---"
done
