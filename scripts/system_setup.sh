#!/bin/bash
# 系统优化脚本 - 部署前执行

set -e

echo "========================================"
echo "  System Optimization Script"
echo "========================================"

# 检查 root 权限
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo)"
    exit 1
fi

# 1. 提高文件描述符限制
echo "[1/6] Configuring file descriptor limits..."
cat >> /etc/security/limits.conf <<'EOF'
* soft nofile 1000000
* hard nofile 1000000
* soft nproc 65535
* hard nproc 65535
EOF

# 2. 调整 TCP 参数
echo "[2/6] Configuring TCP parameters..."
cat >> /etc/sysctl.conf <<'EOF'
# TCP Time-Wait 重用
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_tw_recycle = 1

# TCP 连接队列
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_max_syn_backlog = 65535

# TCP 内存优化
net.ipv4.tcp_mem = 94500000 915000000 927000000
net.ipv4.tcp_rmem = 4096 87380 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216

# 其他优化
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.ipv4.ip_local_port_range = 1024 65535
net.ipv4.tcp_fin_timeout = 15
net.ipv4.tcp_keepalive_time = 300
net.ipv4.tcp_keepalive_probes = 3
net.ipv4.tcp_keepalive_intvl = 15

# 禁用慢启动重启
net.ipv4.tcp_slow_start_after_idle = 0

# IP 转发
net.ipv4.ip_forward = 1
EOF

sysctl -p > /dev/null 2>&1 || true

# 3. 关闭透明大页
echo "[3/6] Disabling transparent hugepages..."
if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
    echo never > /sys/kernel/mm/transparent_hugepage/enabled
fi
if [ -f /sys/kernel/mm/transparent_hugepage/defrag ]; then
    echo never > /sys/kernel/mm/transparent_hugepage/defrag
fi

# 4. 设置 CPU 调度
echo "[4/6] Setting CPU scheduler..."
echo "performance" > /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null || true

# 5. 创建日志目录
echo "[5/6] Creating directories..."
mkdir -p /var/log/high_perf_server
mkdir -p /var/lib/high_perf_server
mkdir -p /etc/high_perf_server

# 6. 安装 liburing
echo "[6/6] Installing dependencies..."
if command -v apt-get > /dev/null; then
    apt-get update -qq
    apt-get install -y -qq liburing-dev cmake g++ > /dev/null 2>&1
elif command -v yum > /dev/null; then
    yum install -y -q liburing-devel cmake gcc-c++ > /dev/null 2>&1
elif command -v dnf > /dev/null; then
    dnf install -y -q liburing-devel cmake gcc-c++ > /dev/null 2>&1
fi

echo "========================================"
echo "  Optimization completed!"
echo "========================================"
echo ""
echo "Recommended: Reboot the system for full effect"
echo "Run: systemctl enable high_perf_server"
