#!/usr/bin/env bash
# 一键切换：20261025 版本 + 图传开启 + 开机自启
#
# 前置条件（缺一不可）：
#   1. 电控已按《双摄像头-电控改造说明.md》完成：USB 分流 + 转发 VT03
#   2. 电控侧验证通过：假帧能原样转发到 VT03 串口（309 字节逐字节一致）
#
# 作用：
#   - 把 configs/standard3.yaml 的 video_send_enable 改为 true
#   - 创建并启用 autoaim-20261025.service（开机自启，跑新版本+图传）
#   - 停用旧的 autoaim.service（指向 20261013，已备份配置）
#   - 启动后等待 20 秒打印状态，确认在跑
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

echo "=== 即将执行 ==="
echo "  版本目录 : $ROOT"
echo "  配置文件 : $CONFIG"
echo "  服务名   : $SERVICE"
echo "  图传     : 由 false 改为 true"
echo ""
read -p "电控改造与假帧转发验证都已完成？(输入 yes 继续) " ans
[[ "$ans" == "yes" ]] || { echo "已取消。"; exit 1; }

# ---------- 1. 备份并打开图传 ----------
cp "$CONFIG" "$BACKUP"
echo "✅ 配置已备份 → $BACKUP"
sed -i 's/^video_send_enable: false/video_send_enable: true/' "$CONFIG"
grep -n "^video_send_enable" "$CONFIG"

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
echo "回滚命令：sudo bash $ROOT/scripts/rollback_to_20261013.sh"
