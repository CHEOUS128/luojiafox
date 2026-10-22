#!/usr/bin/env bash
# 回滚：关闭图传 + 服务切回 20261013
#
# 用法：
#   sudo bash scripts/rollback_to_20261013.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="$ROOT/configs/standard3.yaml"

if [[ $EUID -ne 0 ]]; then
  echo "请用 sudo 运行：sudo bash $0"
  exit 1
fi

echo "=== 回滚到 20261013（关闭图传）==="

# 1. 关掉新服务
systemctl stop autoaim-20261025 2>/dev/null || true
systemctl disable autoaim-20261025 2>/dev/null || true

# 2. 关闭图传，并把协议退回旧版（20261013 的服务用不到，但保持一致更安全）
if grep -q '^video_send_enable: true' "$CONFIG"; then
  sed -i 's/^video_send_enable: true/video_send_enable: false/' "$CONFIG"
  echo "✅ 图传已关闭"
fi

# 3. 切回旧服务
systemctl daemon-reload
systemctl enable autoaim
systemctl start autoaim

sleep 5
systemctl is-active autoaim && echo "✅ 已回到 20261013 版本（服务 autoaim）"
echo ""
echo "如需恢复图传：sudo bash $ROOT/scripts/enable_video_service.sh"
