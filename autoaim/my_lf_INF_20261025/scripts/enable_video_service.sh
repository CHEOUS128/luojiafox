#!/usr/bin/env bash
# 一次性切换：20261025 版本 + 图传开启 + 开机自启
#
# 执行时间：现在（不是等电控改完之后）
# 执行效果：以后每次开机都自动运行 20261025 版本 + 图传，
#           电控把接收侧改好后，上位机这边不需要再做任何操作。
#
# 作用：
#   - 把 configs/standard3.yaml 的 video_send_enable 改为 true（永久生效）
#   - 创建并启用 autoaim-20261025.service（开机自启，跑新版本+图传）
#   - 停用旧的 autoaim.service（指向 20261013）
#   - 启动后等待 20 秒打印状态，确认在跑
#
# ⚠️ 重要：脚本执行后，视频帧就会开始往串口发。
#    在电控刷入新固件之前，309 字节视频帧会被旧主控当作控制帧解析，
#    云台控制会受影响。这段时间不要用这辆车做云台相关测试；
#    如果必须用，先执行 scripts/rollback_to_20261013.sh 回滚。
#
# 用法：
#   sudo bash scripts/enable_video_service.sh
#
# 回滚：
#   sudo bash scripts/rollback_to_20261013.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE=autoaim-20261025
CONFIG="$ROOT/configs/standard3.yaml"
BACKUP="$ROOT/configs/standard3.yaml.bak-$(date +%Y%m%d-%H%M%S)"

if [[ $EUID -ne 0 ]]; then
  echo "请用 sudo 运行：sudo bash $0"
  exit 1
fi

# ---------- 0. 前置检查 ----------
if [[ ! -x "$ROOT/build/standard_mpc" ]]; then
  echo "❌ 找不到 $ROOT/build/standard_mpc，先编译："
  echo "   cd $ROOT/build && make standard_mpc -j8"
  exit 1
fi
if [[ ! -f "$ROOT/assets/yolov5.engine" ]]; then
  echo "❌ 找不到 $ROOT/assets/yolov5.engine（模型文件），从 20261013 拷过来："
  echo "   cp -r ~/my_lf_INF_20261013/assets $ROOT/"
  exit 1
fi
if ! grep -q "video_send_enable" "$CONFIG"; then
  echo "❌ $CONFIG 里没有 video_send_enable，检查配置是否正确。"
  exit 1
fi

echo "=== 即将执行（一次性，之后开机自动生效）==="
echo "  版本目录 : $ROOT"
echo "  配置文件 : $CONFIG"
echo "  服务名   : $SERVICE"
echo "  图传     : 由 false 改为 true（永久）"
echo "  三阶协议 : three_order_enable 强制改为 true"
echo ""
echo "⚠️  电控新固件刷入前，视频帧会影响旧主控的云台控制。"
echo "    这段时间不要用车做云台测试；需要用车请先跑回滚脚本。"
echo ""
read -p "确认现在切换？(输入 yes 继续) " ans
[[ "$ans" == "yes" ]] || { echo "已取消。"; exit 1; }

# ---------- 1. 备份并打开完整功能 ----------
cp "$CONFIG" "$BACKUP"
echo "✅ 配置已备份 → $BACKUP"

# 完整功能必须同时满足：
#   video_send_enable  = true  图传开启
#   three_order_enable = true  三阶协议（下位机 RECEIVE_DATA=32 字节）
sed -i 's/^video_send_enable: .*/video_send_enable: true      # 图传开启/' "$CONFIG"
sed -i 's/^three_order_enable: .*/three_order_enable: true    # 三阶协议：下位机按 32 字节解析/' "$CONFIG"

echo "--- 关键配置确认 ---"
grep -n "^video_send_enable\|^three_order_enable" "$CONFIG"

if ! grep -q "^video_send_enable: true" "$CONFIG" || ! grep -q "^three_order_enable: true" "$CONFIG"; then
  echo "❌ 配置修改失败，请手动检查 $CONFIG"
  exit 1
fi

# ---------- 2. 创建新服务 ----------
cat > "/etc/systemd/system/$SERVICE.service" <<EOF
[Unit]
Description=RoboMaster Auto Aim (lf_INF 20261025, video enabled)
After=multi-user.target

[Service]
Type=simple
User=root
WorkingDirectory=$ROOT
ExecStartPre=/bin/sleep 15
ExecStart=$ROOT/build/standard_mpc configs/standard3.yaml
Restart=always
RestartSec=3
StandardOutput=append:$ROOT/logs/autostart.log
StandardError=append:$ROOT/logs/autostart.log

[Install]
WantedBy=multi-user.target
EOF

mkdir -p "$ROOT/logs"
chmod 755 "$ROOT/logs"

# ---------- 3. 切换服务 ----------
systemctl daemon-reload
systemctl stop autoaim 2>/dev/null || true
systemctl disable autoaim 2>/dev/null || true
systemctl enable "$SERVICE"
systemctl start "$SERVICE"

# ---------- 4. 验证 ----------
echo ""
echo "等待 20 秒（服务内含 15 秒启动延迟）..."
sleep 20
systemctl is-active "$SERVICE" && echo "✅ 服务运行中"
echo ""
echo "=== 最近日志 ==="
tail -n 15 "$ROOT/logs/autostart.log" 2>/dev/null || echo "(暂无日志)"
echo ""
echo "完成。以后开机自动运行：20261025 版本 + 图传开启。"
echo "电控改好后，上位机不需要再做任何操作。"
echo "回滚命令：sudo bash $ROOT/scripts/rollback_to_20261013.sh"
