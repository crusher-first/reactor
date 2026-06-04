#!/bin/bash
# 性能测试脚本
# 需要先安装: apt install wrk

set -e

SERVER_URL="${1:-http://127.0.0.1:8080}"
DURATION="${2:-30s}"
THREADS="${3:-8}"
CONNECTIONS="${4:-100}"

echo "========================================"
echo "  Performance Benchmark"
echo "========================================"
echo "  Server:     $SERVER_URL"
echo "  Duration:  $DURATION"
echo "  Threads:   $THREADS"
echo "  Conn:      $CONNECTIONS"
echo "========================================"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

# 检查 wrk
if ! command -v wrk &> /dev/null; then
    echo "Installing wrk..."
    apt-get update -qq && apt-get install -y -qq wrk > /dev/null 2>&1 || {
        echo "Failed to install wrk. Install manually: apt install wrk"
        exit 1
    }
fi

echo ""
echo "[Test 1] Static File (small)"
wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION} \
    --latency \
    "${SERVER_URL}/index.html"

echo ""
echo "[Test 2] Static File (large, 1MB)"
# 创建 1MB 测试文件
dd if=/dev/zero of=/tmp/test_1mb.bin bs=1M count=1 2>/dev/null
curl -s "${SERVER_URL}/test_1mb.bin" -o /dev/null || true

wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION} \
    --latency \
    "${SERVER_URL}/test_1mb.bin"

echo ""
echo "[Test 3] Concurrent Connections Stress"
wrk -t$((THREADS * 2)) -c$((CONNECTIONS * 4)) -d${DURATION} \
    --latency \
    "${SERVER_URL}/index.html"

echo ""
echo "[Test 4] JSON API (if implemented)"
wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION} \
    --latency \
    -H "Content-Type: application/json" \
    -d '{"test":"data"}' \
    "${SERVER_URL}/api/echo"

echo ""
echo "========================================"
echo "  Benchmark Complete"
echo "========================================"

# 清理
rm -f /tmp/test_1mb.bin
