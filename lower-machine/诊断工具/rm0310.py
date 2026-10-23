#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rm0310.py —— 0x0310 第二视角链路「分段隔离」诊断工具（纯标准库，不用 pip 就能查包）

为什么要有这个工具
------------------
第二视角画面收不到。整条链路是：

  Jetson → USB CDC → STM32 → UART 921600 → VT03 → 无线 → VT13/选手端 → MQTT CustomByteBlock → 客户端
     ✅        ✅        ✅           ✅         ❓        ✅            ✅                 ✅
   已验证   已验证    已验证      已验证    唯一未知      已验证        已验证            已验证

无线那一跳（VT03↔VT13）插不进任何工具，所以**必须把链路拆开、每一段单独验**。
本工具负责其中的三件事：

  1. 灌包   send  ：让一台电脑扮演 STM32，把数据打进 VT03 的 RX，看能不能出现在客户端
  2. 收包   recv  ：让一台电脑扮演"VT13 后面的电脑"，接在 PG14 或 VT03 的 TX 上，看有没有东西出来
  3. 拆包   dump  ：把抓到的原始字节一层层剥开，看每一层到底对不对

关键事实（决定了整个排查思路，都已在源码/协议里核实）
------------------------------------------------------
* 协议 表 1-41：0x0310 = 300 字节 data，频率上限 50Hz，**无重传**。
* 这 300 字节**没有端到端校验**：外层 309 字节的 CRC8/CRC16 在 STM32 那一跳就被校验并剥掉了。
  → "客户端收到了 CustomByteBlock" 只证明包到了，不证明包是对的。
  → **唯一能证明完整性的证据是内层序号连不连续**。本工具就是干这个的。
* 客户端只认 300 字节的包（custom_byte_block_video_decoder.cpp:8  kMaxSupportedPacketBytes=300）。
  → 如果 VT03 把 309 字节原样转给客户端，客户端会直接丢弃（超过 300 字节），
    但 RoboGo 的抓包文件里仍然会记下长度 309 —— 这正好用来判断 VT03 是"透传"还是"解析协议"。

300 字节里到底是什么（与客户端源码严格一致）
--------------------------------------------
客户端 CustomByteBlockVideoDecoder 的读法（custom_byte_block_video_decoder.cpp:84-111, 55-72）：

  data[0:2]  内层长度 BE        （一个包只装 1 个内层包时 = 298）
  data[2:4]  内层序号 BE
  data[4:…]  内层内容（H.264 数据，最多 296 字节）
  data[…:300] 补 0（内层长度为 0 即视为填充截止）

我们的 VideoSender 写的就是这个结构（video_sender.cpp:408-421）。
客户端硬性规则（custom_byte_block_video_decoder.cpp:100-105）：
  内层长度**不能超过 298**，且填充必须是 0 → 长度字段不可能等于 300。
所以"一个 0x0310 的 300 字节 data"里只能装 1 个内层包 + 补 0，没有第二种写法。
若这一层解析失败，客户端才退回 SerializedPacket 口径（[2B 序号][内容]），
对应本工具的 --style direct。

用法（Windows PowerShell / cmd / Linux 通用）
--------------------------------------------
  # 0) 先自检（不用接任何硬件，验证本工具和 CRC/解析都没问题）
  python rm0310.py selftest

  # 1) 列串口
  python rm0310.py ports

  # 2) 实验 2：电脑扮演 STM32，往 VT03 的 RX 灌包
  python rm0310.py send COM5 --style alt --hz 1 --seconds 120
  #    --style alt = 交替发 300B(canon) 和 309B(frame)，一次实验同时验证两种 VT03 行为
  #    另开客户端，事后用第 5 条命令看 network_capture.bin 里的长度分布

  # 3) 实验 3：电脑接在 PG14（或 VT03 的 TX）上收包
  python rm0310.py recv COM7 --seconds 30
  #    会自动识别 309B 的 0x0310 帧 和 21B 的 0xA9 0x53 遥控帧

  # 4) 实验 4：跑真链路数数（Jetson 上把 video_send_rate_hz 改成 2.0）
  #    不打断 RoboGo，事后拆它自己的抓包：
  python rm0310.py dump "C:\\路径\\logs\\<会话>\\network_capture.bin"

  # 5) 不装客户端、不踢掉 RoboGo 之外的任何东西，直接看 MQTT 上有没有包
  python rm0310.py probe --robot 3 --seconds 60
  #    注意：clientID 会顶掉同 ID 的客户端，跑之前先关 RoboGo

  # 6) 生成一个 300B 包文件，给「发送假包.bat」(PowerShell) 用
  python rm0310.py gen pkts.bin --count 200 --style canon

注意
----
* send / recv 需要 pyserial（pip install pyserial）。dump / gen / probe / selftest 不需要。
* 灌包的波特率必须是 921600（协议 表 1-1 + VT03 说明书）。
* 不要用 USB-TTL 的 5V 给 VT03 供电，VT03 必须用它自己的 12V 供电。
"""

from __future__ import print_function

import argparse
import datetime
import json
import os
import socket
import struct
import sys
import time

# --------------------------------------------------------------------------
# 常量（对齐《RoboMaster 2026 通信协议 V2.0.0》表 1-1 / 表 1-41 / 附录一）
# --------------------------------------------------------------------------
VIDEO_DATA_BYTES = 300          # 0x0310 的 data 长度
VIDEO_FRAME_BYTES = 309         # 串口帧：A5 + len(2) + seq + crc8 + cmd(2) + 300 + crc16(2)
SOF = 0xA5
CMD_ID = 0x0310
BAUD = 921600

RM_MAGIC = b"RM26"              # 我方可识别的标记，放在内层内容开头
INNER_CANON_LEN = 298           # [2B seq][296B 内容]；协议上不能超过 298

RC_SOF = b"\xa9\x53"            # VT03 每 14ms 输出的 21 字节遥控帧（说明书 p.15）
RC_FRAME_BYTES = 21

# RoboGo 的日志目录（客户端会在 <exe 所在目录>/logs/<会话>/ 下写 robogo.log）。
# 探针跑完会去这里找"同一时间段客户端是不是也在场、它自己收到了多少包"，
# 用来区分"包没到客户端"和"到了但客户端解不出来"。
ROBO_CLIENT_LOG_DIRS = [
    r"C:\Users\CH\Desktop\robot记录\luojiafox\luojiamaster\源码\build\release\logs",
    r"C:\Users\CH\Desktop\robot记录\luojiafox\luojiamaster\程序\logs",
]

# ------------------------------ CRC 表 ------------------------------------
CRC8_TAB = bytes.fromhex(
    "00 5e bc e2 61 3f dd 83 c2 9c 7e 20 a3 fd 1f 41"
    "9d c3 21 7f fc a2 40 1e 5f 01 e3 bd 3e 60 82 dc"
    "23 7d 9f c1 42 1c fe a0 e1 bf 5d 03 80 de 3c 62"
    "be e0 02 5c df 81 63 3d 7c 22 c0 9e 1d 43 a1 ff"
    "46 18 fa a4 27 79 9b c5 84 da 38 66 e5 bb 59 07"
    "db 85 67 39 ba e4 06 58 19 47 a5 fb 78 26 c4 9a"
    "65 3b d9 87 04 5a b8 e6 a7 f9 1b 45 c6 98 7a 24"
    "f8 a6 44 1a 99 c7 25 7b 3a 64 86 d8 5b 05 e7 b9"
    "8c d2 30 6e ed b3 51 0f 4e 10 f2 ac 2f 71 93 cd"
    "11 4f ad f3 70 2e cc 92 d3 8d 6f 31 b2 ec 0e 50"
    "af f1 13 4d ce 90 72 2c 6d 33 d1 8f 0c 52 b0 ee"
    "32 6c 8e d0 53 0d ef b1 f0 ae 4c 12 91 cf 2d 73"
    "ca 94 76 28 ab f5 17 49 08 56 b4 ea 69 37 d5 8b"
    "57 09 eb b5 36 68 8a d4 95 cb 29 77 f4 aa 48 16"
    "e9 b7 55 0b 88 d6 34 6a 2b 75 97 c9 4a 14 f6 a8"
    "74 2a c8 96 15 4b a9 f7 b6 e8 0a 54 d7 89 6b 35"
)

CRC16_TAB = [
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78,
]


def crc8(data):
    c = 0xFF
    for b in bytearray(data):
        c = CRC8_TAB[c ^ b]
    return c


def crc16(data):
    c = 0xFFFF
    for b in bytearray(data):
        c = ((c >> 8) ^ CRC16_TAB[(c ^ b) & 0xFF]) & 0xFFFF
    return c


# --------------------------------------------------------------------------
# 构造
# --------------------------------------------------------------------------
def make_payload_300(seq, beat):
    """我方口径的 300 字节 CustomByteBlock data（与 video_sender.cpp 一致）：

        [2B 内层长度 BE = 298][2B 内层序号 BE][4B 'RM26'][4B 心跳][288B 0]

    客户端会把它当成一个内层包，序号在 offset 2，内容从 offset 4 开始。
    """
    pkt = bytearray(VIDEO_DATA_BYTES)
    struct.pack_into(">H", pkt, 0, INNER_CANON_LEN)
    struct.pack_into(">H", pkt, 2, seq & 0xFFFF)
    pkt[4:8] = RM_MAGIC
    struct.pack_into(">I", pkt, 8, beat & 0xFFFFFFFF)
    return bytes(pkt)


def make_direct_300(seq, beat):
    """裸序号口径（客户端解不出内层长度时的 fallback）：

        [2B 序号 BE][4B 'RM26'][4B 心跳][…]

    用来测『长度字段那 2 个字节是不是在路上被吃掉了』。
    """
    pkt = bytearray(VIDEO_DATA_BYTES)
    struct.pack_into(">H", pkt, 0, seq & 0xFFFF)
    pkt[2:6] = RM_MAGIC
    struct.pack_into(">I", pkt, 6, beat & 0xFFFFFFFF)
    return bytes(pkt)


def make_frame_309(seq, beat):
    """完整 309 字节 0x0310 串口帧 = STM32 从 PG14 发出的原始形态"""
    f = bytearray(VIDEO_FRAME_BYTES)
    f[0] = SOF
    f[1] = VIDEO_DATA_BYTES & 0xFF
    f[2] = (VIDEO_DATA_BYTES >> 8) & 0xFF
    f[3] = seq & 0xFF
    f[4] = crc8(f[0:4])
    f[5] = CMD_ID & 0xFF
    f[6] = (CMD_ID >> 8) & 0xFF
    f[7:307] = make_payload_300(seq, beat)
    c = crc16(f[0:307])
    f[307] = c & 0xFF
    f[308] = (c >> 8) & 0xFF
    return bytes(f)


def build_packet(style, seq, beat):
    if style == "frame":
        return make_frame_309(seq, beat)
    if style == "direct":
        return make_direct_300(seq, beat)
    return make_payload_300(seq, beat)


STYLE_DESC = {
    "canon":  "300B  我方口径 [len=298][seq][RM26…]（= 客户端里的 CustomByteBlock data）",
    "direct": "300B  裸序号 [seq][RM26…]（没有长度字段，测『长度字段是不是被吃掉了』）",
    "frame":  "309B  完整 0x0310 帧（= STM32 从 PG14 发出的原始字节）",
    "alt":    "交替发 canon(300B) 和 frame(309B)，一次实验覆盖两种 VT03 转发方式",
}

STYLE_ORDER = ["canon", "frame"]


# --------------------------------------------------------------------------
# 解析
# --------------------------------------------------------------------------
def _classify(payload):
    """识别内层内容是什么（只给人看，不影响统计）"""
    if len(payload) >= 8 and payload[:4] == RM_MAGIC:
        return "RM26", struct.unpack_from(">I", payload, 4)[0]
    head = payload
    if head[:4] == b"\x00\x00\x00\x01":
        head = head[4:]
    elif head[:3] == b"\x00\x00\x01":
        head = head[3:]
    elif head[:4] == b"\x00\x00\x00\x00":
        return "全0", None
    if head:
        nal = head[0] & 0x1F
        if nal == 5:
            return "H264 IDR", None
        if nal == 1:
            return "H264 P帧", None
        if nal == 7:
            return "H264 SPS", None
        if nal == 8:
            return "H264 PPS", None
    if payload and not any(payload):
        return "全0", None
    return "未知", None


def parse_payload_300(data, style="auto"):
    """把 300 字节的 CustomByteBlock data 拆成内层包。

    返回 (pkts, notes)。pkt = {"seq","len","payload_bytes","magic","beat"}
    """
    pkts, notes = [], []
    data = bytes(data)
    if not data:
        return pkts, ["空包（客户端会跳过 data 为空的 CustomByteBlock）"]
    if len(data) != VIDEO_DATA_BYTES:
        notes.append("长度 %d != 300（客户端只把 300 字节当 transport bundle 解析）" % len(data))

    def _add(seq, payload, ln):
        magic, beat = _classify(payload)
        pkts.append({"seq": seq, "len": ln, "payload_bytes": len(payload),
                     "magic": magic, "beat": beat})

    def _as_direct():
        _add(struct.unpack_from(">H", data, 0)[0], data[2:], len(data))

    if style == "direct":
        _as_direct()
        return pkts, notes

    # canon / direct / auto：canon 与客户端默认的 transport 解析路径完全一致
    off = 0
    parsed_ok = True
    while off + 2 <= len(data):
        ln = struct.unpack_from(">H", data, off)[0]
        off += 2
        if ln == 0:
            if any(data[off:]):
                notes.append("内层长度为 0 但后面还有 %d 字节非 0 数据" % (len(data) - off))
            break
        if ln < 2:
            parsed_ok = False
            notes.append("非法内层长度 %d（<2）" % ln)
            break
        if ln > 298:
            parsed_ok = False
            notes.append("内层长度 %d 超过 298（客户端直接丢弃整包）" % ln)
            break
        if off + ln > len(data):
            parsed_ok = False
            notes.append("内层长度 %d 超过剩余 %d 字节（包被截断）" % (ln, len(data) - off))
            break
        body = data[off:off + ln]
        off += ln
        # 客户端 deserialize_serialized_packet: 前 2 字节 = 序号，其余 = 内容
        _add(struct.unpack_from(">H", body, 0)[0], body[2:], ln)

    if style == "auto" and not parsed_ok and not pkts:
        notes.append("按客户端默认口径解不出来，回退成『裸序号』口径")
        _as_direct()
    return pkts, notes


def parse_0x0310_stream(buf):
    """在连续字节流里找 309 字节的 0x0310 帧（不做 CRC 门禁，是要看哪一层坏）。

    返回 (frames, junk_bytes)
    """
    frames = []
    i, n, junk = 0, len(buf), 0
    while i < n:
        if buf[i] != SOF:
            i += 1
            junk += 1
            continue
        if i + 7 > n:
            break
        length = buf[i + 1] | (buf[i + 2] << 8)
        cmd = buf[i + 5] | (buf[i + 6] << 8)
        if length != VIDEO_DATA_BYTES or cmd != CMD_ID:
            i += 1
            junk += 1
            continue
        total = 7 + length + 2
        if i + total > n:
            break
        f = buf[i:i + total]
        frames.append({
            "outer_seq": f[3],
            "crc8_ok": crc8(f[0:4]) == f[4],
            "crc16_ok": crc16(f[0:7 + length]) == (f[7 + length] | (f[8 + length] << 8)),
            "data": bytes(f[7:7 + length]),
        })
        i += total
    return frames, junk


def seq_stats(seqs):
    """序号连续性：重复 / 丢 / 乱序（16 位回绕安全）"""
    out = {"count": len(seqs), "unique": len(set(seqs)), "dups": [], "reorder": 0,
           "lost": 0, "lost_list": []}
    if not seqs:
        return out
    cnt = {}
    for s in seqs:
        cnt[s] = cnt.get(s, 0) + 1
    out["dups"] = [s for s, c in cnt.items() if c > 1][:20]
    lost = 0
    lost_list = []
    for a, b in zip(seqs, seqs[1:]):
        d = (b - a) & 0xFFFF
        if d == 0:
            continue
        if d > 0x8000:
            out["reorder"] += 1
            continue
        if d > 1:
            lost += d - 1
            if len(lost_list) < 20:
                lost_list.append((a + 1, b - 1, d - 1))
    out["lost"] = lost
    out["lost_list"] = lost_list
    return out


def _decode_cbb(payload):
    """解 CustomByteBlock 的 protobuf：message CustomByteBlock { bytes data = 1; }

    tag = 0x0A（字段1，wire type 2），后面跟 varint 长度。
    返回 (data_bytes, note)
    """
    if not payload:
        return b"", "空 protobuf"
    if payload[0] != 0x0A:
        return payload, "payload[0]=0x%02x 不是 0x0A（按裸字节处理）" % payload[0]
    i, shift, ln = 1, 0, 0
    while i < len(payload):
        b = payload[i]
        i += 1
        ln |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            break
    else:
        return b"", "varint 长度没读完"
    data = payload[i:i + ln]
    note = ""
    if len(data) != ln:
        note = "声明长度 %d，实际只有 %d 字节" % (ln, len(data))
    elif i + ln != len(payload):
        note = "字段1 之后还有 %d 字节（客户端会忽略）" % (len(payload) - i - ln)
    return data, note


# --------------------------------------------------------------------------
# 串口
# --------------------------------------------------------------------------
def list_ports():
    try:
        from serial.tools import list_ports as lp
        return [(p.device, p.description, getattr(p, "hwid", "")) for p in lp.comports()], "pyserial"
    except Exception:
        pass
    if sys.platform.startswith("win"):
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
            ports, i = [], 0
            while True:
                try:
                    name, val, _ = winreg.EnumValue(key, i)
                    ports.append((val, name, ""))
                    i += 1
                except OSError:
                    break
            return ports, "winreg"
        except Exception as e:
            return [], "winreg 枚举失败: %s" % e
    import glob
    devs = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    return [(d, "", "") for d in devs], "glob"


def open_serial(port, baud):
    try:
        import serial
    except Exception:
        sys.exit("需要 pyserial：\n"
                 "  有网：  python -m pip install pyserial\n"
                 "  没网：  在有网的电脑上 pip download pyserial -d . 然后把 whl 拷过来\n"
                 "          python -m pip install pyserial-*.whl\n"
                 "（只做 dump / gen / probe / selftest 的话不需要 pyserial）")
    sp = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1,
                       timeout=0.1, write_timeout=1.0)
    sp.reset_input_buffer()
    return sp


# --------------------------------------------------------------------------
# 子命令：ports
# --------------------------------------------------------------------------
def cmd_ports(args):
    ports, src = list_ports()
    print("枚举方式: %s" % src)
    if not ports:
        print("没找到串口。检查：")
        print("  * 设备插上了吗？设备管理器里有黄色感叹号吗？")
        print("  * USB-TTL 需要 CH340 / CP210x / FT232 驱动；VT13 需要 DJI USB 驱动")
        return 1
    print("%-10s %-46s %s" % ("端口", "描述", "硬件ID"))
    for dev, desc, hwid in ports:
        print("%-10s %-46s %s" % (dev, desc, hwid))
    return 0


# --------------------------------------------------------------------------
# 子命令：send
# --------------------------------------------------------------------------
def cmd_send(args):
    styles = STYLE_ORDER if args.style == "alt" else [args.style]
    print("=== 灌包模式（电脑扮演 STM32 / 图传发送端）===")
    print("端口=%s 波特率=%d 频率=%.2fHz 时长=%ss" % (args.port, args.baud, args.hz, args.seconds))
    print("发的内容:")
    for s in styles:
        print("  %-6s %s" % (s, STYLE_DESC[s]))
    print("")
    print("接线要求: PC 的 TX → VT03 的 RX，PC 的 GND → VT03 的 GND")
    print("          STM32 的 PG14 必须先断开（否则两个发送端顶在一起）")
    print("          VT03 用他自己的 12V 供电，不要用 USB-TTL 的 5V")
    print("")

    sp = open_serial(args.port, args.baud)
    period = 1.0 / args.hz
    start = time.time()
    seq = args.start_seq & 0xFFFF
    sent = 0
    next_report = start + 5.0
    rc_frames = 0
    rx_junk = 0

    try:
        while True:
            now = time.time()
            if now - start > args.seconds:
                break
            if args.count and sent >= args.count:
                break
            style = styles[sent % len(styles)]
            beat = int(now - start)
            pkt = build_packet(style, seq, beat)
            sp.write(pkt)
            sent += 1
            if sent <= 4 or args.verbose:
                print("  发 %-6s seq=%5d beat=%d %3dB  %s" % (
                    style, seq, beat, len(pkt), pkt[:12].hex(" ")))
            seq = (seq + 1) & 0xFFFF

            # 顺手看一下 VT03 有没有在回 21 字节遥控帧（说明它活着、且 TX 线接在你这里）
            try:
                if sp.in_waiting:
                    raw = sp.read(sp.in_waiting)
                    rc_frames += raw.count(RC_SOF)
                    if not raw.count(RC_SOF):
                        rx_junk += len(raw)
            except Exception:
                pass

            if now >= next_report:
                el = now - start
                print("[%5.1fs] 已发 %d 包 (%.2f/s)  回读: 遥控帧 %d  其他 %dB" % (
                    el, sent, sent / el, rc_frames, rx_junk))
                next_report += 5.0

            sleep = (start + sent * period) - time.time()
            if sleep > 0.0005:
                time.sleep(sleep)
    except KeyboardInterrupt:
        print("\n[手动中断]")
    finally:
        sp.close()

    el = time.time() - start
    print("\n=== 灌包结束 ===")
    print("  共发 %d 包，%.1f 秒，%.2f 包/秒" % (sent, el, sent / el if el else 0))
    print("  序号 %d … %d" % (args.start_seq, (args.start_seq + sent - 1) & 0xFFFF))
    print("  回读到的 21B 遥控帧: %d 个" % rc_frames)
    if rc_frames > 0:
        print("    → VT03 的 TX 引脚确实接到你的 PC 上了（不然读不到），且它在对频/已连上")
    else:
        print("    → 没读到遥控帧：要么你的 RX 没接 VT03 的 TX（正常，做这个实验只需接 TX→RX），")
        print("      要么 VT03 没上电/没对频。")
    print("")
    print("  【判读】现在去客户端看 300 秒内的结果（不要关模拟器）：")
    print("    客户端里出现第二视角画面 / 日志出现 CustomByteBlock      → VT03 无线这一跳是好的")
    print("    network_capture.bin 里有 300 字节的包                   → 同上（用 dump 确认序号连续）")
    print("    只有 309 字节的包、没有 300 的                        → VT03 在透传原始帧，不剥外壳")
    print("       → 那么真链路的断点在 STM32 之前，不在 VT03")
    print("    一条 CustomByteBlock 都没有                            → VT03→VT13→选手端 这一跳断了")
    print("       → 查 VT03 指示灯（绿=已连 / 红=未连接收端 / 蓝=对频中 / 紫=未激活）")
    return 0


# --------------------------------------------------------------------------
# 子命令：recv
# --------------------------------------------------------------------------
def cmd_recv(args):
    print("=== 收包模式（电脑扮演 VT13 后面的电脑 / 示波器替代品）===")
    print("端口=%s 波特率=%d 时长=%ss" % (args.port, args.baud, args.seconds))
    print("自动识别: 309B 的 0x0310 视频帧 / 21B 的 0xA9 0x53 遥控帧 / 300B 裸包")
    print("")
    sp = open_serial(args.port, args.baud)
    buf = bytearray()
    frames = []          # 0x0310 帧
    inner_seqs = []
    outer_seqs = []
    rc_count = 0
    junk = 0
    beats = []
    raw_seqs = []        # 裸 300B 包
    saved = open(args.save, "wb") if args.save else None
    start = time.time()
    next_report = start + 2.0
    last_ts = None
    gaps = []

    try:
        while time.time() - start < args.seconds:
            chunk = sp.read(max(1, sp.in_waiting or 1))
            if not chunk:
                continue
            buf.extend(chunk)

            # --- 21B 遥控帧 ---
            rc_count += bytes(chunk).count(RC_SOF)

            # --- 309B 0x0310 帧 ---
            while True:
                idx = buf.find(b"\xa5")
                if idx < 0:
                    junk += len(buf)
                    del buf[:]
                    break
                if idx > 0:
                    junk += idx
                    del buf[:idx]
                if len(buf) < 3:
                    break
                ln = buf[1] | (buf[2] << 8)
                if ln != VIDEO_DATA_BYTES:
                    junk += 1
                    del buf[:1]
                    continue
                if len(buf) < VIDEO_FRAME_BYTES:
                    break
                raw = bytes(buf[:VIDEO_FRAME_BYTES])
                del buf[:VIDEO_FRAME_BYTES]
                cmd = raw[5] | (raw[6] << 8)
                if cmd != CMD_ID:
                    junk += 1
                    continue
                outer_seqs.append(raw[3])
                pkts, notes = parse_payload_300(raw[7:307], "auto")
                for p in pkts:
                    inner_seqs.append(p["seq"])
                    if p["beat"] is not None:
                        beats.append(p["beat"])
                if saved:
                    saved.write(raw[7:307])
                now = time.time()
                if last_ts is not None:
                    gaps.append(now - last_ts)
                last_ts = now
                if len(frames) < 5 or args.verbose:
                    print("  [%7.3fs] 0x0310 seq=%3d crc8=%s crc16=%s 内层=%s" % (
                        now - start, raw[3],
                        "ok" if crc8(raw[0:4]) == raw[4] else "BAD",
                        "ok" if crc16(raw[0:307]) == (raw[307] | (raw[308] << 8)) else "BAD",
                        ",".join(str(p["seq"]) for p in pkts) or "无"))
                frames.append(raw)

            if time.time() >= next_report:
                st = seq_stats(inner_seqs)
                el = time.time() - start
                print("[%5.1fs] 0x0310帧=%d 内层包=%d 唯一=%d 丢=%d 重复=%d | 遥控帧=%d 垃圾=%dB" % (
                    el, len(frames), st["count"], st["unique"], st["lost"],
                    max(0, st["count"] - st["unique"]), rc_count, junk))
                next_report += 2.0
    except KeyboardInterrupt:
        print("\n[手动中断]")
    finally:
        sp.close()
        if saved:
            saved.close()

    el = time.time() - start
    print("\n=== 接收结果（%.1f 秒）===" % el)
    print("  0x0310 帧: %d 个  (%.2f 帧/秒)" % (len(frames), len(frames) / el if el else 0))
    if frames:
        bad8 = sum(1 for f in frames if crc8(f[0:4]) != f[4])
        bad16 = sum(1 for f in frames if crc16(f[0:307]) != (f[307] | (f[308] << 8)))
        print("  CRC8 错: %d   CRC16 错: %d" % (bad8, bad16))
        st = seq_stats(outer_seqs)
        print("  外层序号: 唯一=%d 丢=%d 重复=%d 乱序=%d" % (
            st["unique"], st["lost"], max(0, st["count"] - st["unique"]), st["reorder"]))
        si = seq_stats(inner_seqs)
        print("  内层序号: 包=%d 唯一=%d 丢=%d 重复=%d" % (
            si["count"], si["unique"], si["lost"], max(0, si["count"] - si["unique"])))
        print("  内层序号缺口: %s" % (si["lost_list"] or "无"))
        if beats:
            print("  心跳: 首=%d 末=%d（正常应≈运行秒数）" % (beats[0], beats[-1]))
        if gaps:
            gaps_sorted = sorted(gaps)
            print("  包间隔: 中位 %.1fms  最大 %.1fms" % (
                1000 * gaps_sorted[len(gaps_sorted) // 2], 1000 * max(gaps)))
    print("  21B 遥控帧: %d 个  (%.2f 帧/秒，正常应对频后一直有，约 71 帧/秒)" % (
        rc_count, rc_count / el if el else 0))
    print("  无法识别而丢弃的字节: %d" % junk)
    if args.save:
        print("  已把收到的 300B 负载存到: %s（可再 dump 一次）" % args.save)
    print("")
    if len(frames) > 0:
        print("  【判读】这个口上确实有 0x0310 数据 → 上游（STM32/Jetson）在发。")
        print("         那么断点在这一跳之后：VT03 的 RX 没接对 / VT03 无线 / VT13 / 选手端。")
    elif rc_count > 0:
        print("  【判读】只有 21B 遥控帧、没有 0x0310 → 你接的是 VT03 的 TX（或 C 板的 PG9）。")
        print("         VT03 活着、在对频，但它没有收到任何 0x0310 → STM32 的 PG14 没发出东西，")
        print("         或者 PG14 那根线没接到 VT03 的 RX。")
    else:
        print("  【判读】什么都没有 → 线没接对 / 波特率不对 / 这个口不是信号线。")
        print("         先确认接线：PC 的 RX ← 对方板的 TX，GND 必须共地。")
    return 0


# --------------------------------------------------------------------------
# 子命令：dump
# --------------------------------------------------------------------------
def _print_pkts(pkts, notes, verbose=False, limit=8):
    for t in notes:
        print("    ! %s" % t)
    for p in pkts[:limit]:
        print("    内层 len=%d seq=%d 内容=%dB 类型=%s%s" % (
            p["len"], p["seq"], p["payload_bytes"], p["magic"],
            " beat=%d" % p["beat"] if p["beat"] is not None else ""))
    if len(pkts) > limit:
        print("    …（还有 %d 个内层包未打印）" % (len(pkts) - limit))
    if not pkts and not notes:
        print("    （没有内层包）")


def cmd_dump(args):
    if not os.path.exists(args.path):
        print("文件不存在: %s" % args.path)
        return 1
    with open(args.path, "rb") as fh:
        blob = fh.read()
    print("文件 %s  %d 字节" % (args.path, len(blob)))

    # ---------- RoboGo 自己的抓包 ----------
    if blob[:8] == b"RBNCAP1\n":
        return _dump_robo_capture(blob, args)

    # ---------- 0x0310 字节流 ----------
    if blob[:1] == b"\xa5" or args.frame:
        frames, junk = parse_0x0310_stream(blob)
        print("\n=== 0x0310 字节流 ===")
        print("  找到帧: %d   无法识别的字节: %d" % (len(frames), junk))
        if not frames:
            print("  ! 一个 0x0310 帧都没有。要么文件不是这个口抓的，要么波特率/接线不对。")
            return 0
        bad8 = sum(1 for f in frames if not f["crc8_ok"])
        bad16 = sum(1 for f in frames if not f["crc16_ok"])
        print("  CRC8 错: %d   CRC16 错: %d" % (bad8, bad16))
        print("  外层序号: %s" % [f["outer_seq"] for f in frames[:16]])
        all_inner = []
        for idx, f in enumerate(frames[:max(1, args.max)]):
            pkts, notes = parse_payload_300(f["data"], args.style)
            print("\n  [帧 %d] outer_seq=%d crc=%s/%s" % (
                idx, f["outer_seq"],
                "ok" if f["crc8_ok"] else "BAD", "ok" if f["crc16_ok"] else "BAD"))
            _print_pkts(pkts, notes, args.verbose)
        for f in frames:
            pkts, _ = parse_payload_300(f["data"], args.style)
            all_inner.extend(pkts)
        if all_inner:
            st = seq_stats([p["seq"] for p in all_inner])
            print("\n=== 内层序号统计（全部 %d 帧）===" % len(frames))
            print("  包=%d 唯一=%d 丢=%d 重复=%d 乱序=%d" % (
                st["count"], st["unique"], st["lost"],
                max(0, st["count"] - st["unique"]), st["reorder"]))
            print("  缺口: %s" % (st["lost_list"] or "无"))
        return 0

    # ---------- JSONL ----------
    if args.jsonl:
        seqs, raws = [], []
        with open(args.path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                try:
                    j = json.loads(line)
                except Exception:
                    continue
                for p in j.get("pkts", []):
                    seqs.append(p["seq"])
                    raws.append(p)
        st = seq_stats(seqs)
        print("\n=== JSONL 统计 ===")
        print("  包=%d 唯一=%d 丢=%d 重复=%d" % (
            st["count"], st["unique"], st["lost"], max(0, st["count"] - st["unique"])))
        print("  缺口: %s" % (st["lost_list"] or "无"))
        for p in raws[:args.max]:
            print("    seq=%d len=%d 内容=%dB 类型=%s" % (
                p["seq"], p["len"], p["payload_bytes"], p["magic"]))
        return 0

    # ---------- 定长 300B 包文件（gen 生成的 / 从抓包里提出来的） ----------
    wrap = args.wrap or VIDEO_DATA_BYTES
    n = len(blob) // wrap
    print("\n=== 按 %d 字节/包 拆分：共 %d 包 ===" % (wrap, n))
    if len(blob) % wrap:
        print("  ! 文件长度不是 %d 的整数倍（可能被截断）" % wrap)
    all_pkts = []
    for i in range(n):
        data = blob[i * wrap:(i + 1) * wrap]
        pkts, notes = parse_payload_300(data, args.style)
        if i < args.max:
            print("\n[包 %d] %s" % (i, data[:16].hex(" ")))
            _print_pkts(pkts, notes, args.verbose)
        all_pkts.extend(pkts)
    if n > args.max:
        print("\n…（共 %d 包，只详列了前 %d 包，统计用全部）" % (n, args.max))
    if all_pkts:
        st = seq_stats([p["seq"] for p in all_pkts])
        print("\n=== 序号统计 ===")
        print("  包=%d 唯一=%d 丢=%d 重复=%d 乱序=%d" % (
            st["count"], st["unique"], st["lost"],
            max(0, st["count"] - st["unique"]), st["reorder"]))
        print("  缺口: %s" % (st["lost_list"] or "无"))
        hist = {}
        for p in all_pkts:
            hist[p["len"]] = hist.get(p["len"], 0) + 1
        print("  内层长度分布: %s" % hist)
        kinds = {}
        for p in all_pkts:
            kinds[p["magic"]] = kinds.get(p["magic"], 0) + 1
        print("  内容类型: %s" % kinds)
    return 0


def _dump_robo_capture(blob, args):
    """解析 RoboGo 自己的 network_capture.bin

    文件头: 'RBNCAP1\\n' + u32 version + u32 flags
    记录:   'REC2' + u32 body_size + u32 crc32 + body
    body:   u64 t_us | u8 transport | u8 dir | u16 chlen | u16 peerlen |
            u32 plen | channel | peer | payload
    transport: 1=mqtt 2=udp-video ; dir: 1=rx 2=tx
    """
    print("\n=== RoboGo 网络抓包 ===")
    off = 8
    ver, flags = struct.unpack_from("<II", blob, off)
    off += 8
    print("  版本=%d flags=0x%08x" % (ver, flags))

    recs = []
    bad = 0
    while off + 12 <= len(blob):
        if blob[off:off + 4] != b"REC2":
            bad += 1
            break
        body_size = struct.unpack_from("<I", blob, off + 4)[0]
        off += 12
        body = blob[off:off + body_size]
        off += body_size
        try:
            # 头 = u64 + u8 + u8 + u16 + u16 + u32 = 8+1+1+2+2+4 = 18 字节
            t_us, transport, direction, chlen, peerlen, plen = struct.unpack_from("<QBBHHI", body, 0)
            p = 18
            channel = body[p:p + chlen].decode("utf-8", "replace")
            p += chlen
            peer = body[p:p + peerlen].decode("utf-8", "replace")
            p += peerlen
            payload = body[p:p + plen]
        except struct.error:
            continue
        recs.append((t_us, transport, direction, channel, peer, payload))
    if bad:
        print("  ! 记录在偏移附近损坏，只解析了前面的部分（%d 条）" % len(recs))

    if not recs:
        print("  这个文件里没有记录。确认抓包功能开着（日志级别 trace）。")
        return 1

    topics = {}
    for t_us, transport, direction, channel, peer, payload in recs:
        key = "%s/%s/%s" % (
            {1: "mqtt", 2: "udp"}.get(transport, "?"),
            {1: "rx", 2: "tx"}.get(direction, "?"), channel)
        topics[key] = topics.get(key, 0) + 1
    print("\n=== 通道/方向统计 ===")
    for k in sorted(topics, key=lambda x: -topics[x]):
        print("  %-44s %d" % (k, topics[k]))

    cbb = [r for r in recs if r[3] == "CustomByteBlock" and r[2] == 1]
    print("\n=== CustomByteBlock（客户端收到的）===")
    if not cbb:
        print("  ❌ 0 条")
        print("     说明：客户端一个自定义数据包都没收到。")
        print("     客户端本身没问题（否则连血量/状态也不会有），所以断点在选手端或更上游。")
        print("     下一步：做『电脑灌包进 VT03』实验（send），直接判 VT03 这一跳。")
        return 0

    t0 = cbb[0][0]
    print("  ✅ %d 条，时间跨度 %.2f 秒（%.1f 条/秒）" % (
        len(cbb), (cbb[-1][0] - t0) / 1e6, len(cbb) / max(1e-6, (cbb[-1][0] - t0) / 1e6)))

    sizes = {}
    per_len_inner = {}
    all_inner = []
    notes_seen = set()
    for t_us, transport, direction, channel, peer, payload in cbb:
        data, note = _decode_cbb(payload)
        if note:
            notes_seen.add(note)
        sizes[len(data)] = sizes.get(len(data), 0) + 1
        pkts, notes = parse_payload_300(data, args.style)
        if len(data) == VIDEO_DATA_BYTES:     # 只有 300 字节才值得看内层
            for n in notes:
                notes_seen.add(n)
        per_len_inner.setdefault(len(data), 0)
        per_len_inner[len(data)] += len(pkts)
        all_inner.extend(pkts)

    print("\n  负载长度分布（raw data 字节数）: %s" % sizes)
    print("  各长度的内层包数: %s" % per_len_inner)
    for n in notes_seen:
        print("  ! %s" % n)

    if 309 in sizes:
        print("\n  ⚠️ 出现了 309 字节的负载 → VT03 把 0x0310 帧**原样透传**了，没有剥外壳。")
        print("     客户端只认 300 字节，所以 309 会被直接丢掉 → 这就是画面出不来的原因。")
        print("     修法：STM32 转发时只发 300 字节 data（去掉 0xA5 外壳和 CRC），")
        print("           或者 VT03 侧改成解析协议（不可控，走前者）。")

    if all_inner:
        st = seq_stats([p["seq"] for p in all_inner])
        print("\n  内层序号: 包=%d 唯一=%d 丢=%d 重复=%d 乱序=%d" % (
            st["count"], st["unique"], st["lost"],
            max(0, st["count"] - st["unique"]), st["reorder"]))
        print("  缺口: %s" % (st["lost_list"] or "无"))
        kinds = {}
        for p in all_inner:
            kinds[p["magic"]] = kinds.get(p["magic"], 0) + 1
        print("  内容类型: %s" % kinds)
        if kinds.get("RM26"):
            print("\n  ✅ 出现 'RM26' 标记 → 这是 rm0310.py send 灌进去的测试包，不是你自己的视频。")
        if st["count"] >= 10:
            ratio = st["unique"] / float(st["count"])
            if ratio > 0.97:
                print("  ✅ 序号基本连续 → 数据完整到客户端了，问题在解码/显示/切换")
            elif ratio > 0.5:
                print("  ⚠️ 序号有重复/缺失 → 到客户端前就在丢（无线或 STM32 队列）")
            else:
                print("  ⚠️ 大量重复 → 可能是同一个包被反复投递，或者序号字段没在递增")
    else:
        print("\n  ⚠️ 一个内层包都解不出来。")
        if sizes.get(300):
            print("     但确实有 300 字节的包 → 格式和客户端预期不一致。")
            print("     用 --style direct 再 dump 一次看看（说不定长度字段被吃了）。")
    return 0


# --------------------------------------------------------------------------
# 子命令：gen
# --------------------------------------------------------------------------
def cmd_gen(args):
    out = bytearray()
    for i in range(args.count):
        out += build_packet(args.style if args.style != "alt" else STYLE_ORDER[i % 2],
                            args.start_seq + i, i // 4)
    with open(args.out, "wb") as fh:
        fh.write(out)
    print("已生成 %s：%d 包，%d 字节（%s）" % (args.out, args.count, len(out), STYLE_DESC[args.style]))
    if args.style == "frame":
        print("每包 309 字节，可直接用 PowerShell 原样写串口。")
    else:
        print("每包 300 字节。")
    return 0


# --------------------------------------------------------------------------
# 子命令：probe（直接连 MQTT，看 CustomByteBlock）
# --------------------------------------------------------------------------
def _find_robo_sessions(start_ts, end_ts):
    """找 RoboGo 在 [start_ts, end_ts] 期间写过的会话目录（按 robogo.log 的 mtime 判断）。"""
    roots = list(ROBO_CLIENT_LOG_DIRS)
    env = os.environ.get("ROBOGO_LOG_DIRS")
    if env:
        roots = [p for p in env.split(os.pathsep) if p] + roots
    found = []
    for root in roots:
        if not os.path.isdir(root):
            continue
        try:
            names = os.listdir(root)
        except OSError:
            continue
        for name in names:
            sdir = os.path.join(root, name)
            log = os.path.join(sdir, "robogo.log")
            if not os.path.isfile(log):
                continue
            try:
                mtime = os.path.getmtime(log)
            except OSError:
                continue
            if start_ts - 120 <= mtime <= end_ts + 120:
                found.append(sdir)
    return found


def _report_robo_side(start_ts, end_ts):
    """探针结束后，看同一时间窗内客户端自己收到了什么。

    这一步能把两种"没画面"分开：
      * 客户端压根没连上 MQTT / 没收到 CustomByteBlock → 不是客户端解包的问题
      * 客户端收到了，但长度/内层格式不对 → 才是格式或解码的问题
    """
    sessions = _find_robo_sessions(start_ts, end_ts)
    print("")
    print("=== 同一时间窗内客户端自己的证据 ===")
    if not sessions:
        print("  没找到这段时间的 RoboGo 会话（可能客户端没开，或日志不在默认目录）。")
        print("  客户端如果没开，这一轮探针只能证明『官方端 MQTT 上有包』，不能证明客户端侧。")
        return
    for sdir in sessions:
        print("  [会话] %s" % sdir)
        log = os.path.join(sdir, "robogo.log")
        n_ok = n_timeout = n_error = 0
        first_hs = last_hs = None
        try:
            with open(log, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    if "MQTT底层网络协议握手成功" in line:
                        n_ok += 1
                        stamp = line[:19]
                        if first_hs is None:
                            first_hs = stamp
                        last_hs = stamp
                    if "MQTT操作超时" in line:
                        n_timeout += 1
                    if "MQTT运行异常" in line:
                        n_error += 1
        except OSError as e:
            print("    读不了 robogo.log: %s" % e)
            continue
        print("    MQTT 握手成功 %d 次（%s ~ %s），操作超时 %d 次，运行异常 %d 次" % (
            n_ok, first_hs or "-", last_hs or "-", n_timeout, n_error))
        cap = os.path.join(sdir, "network_capture.bin")
        n_rx = None
        if os.path.isfile(cap):
            try:
                n_rx = _count_cbb_rx(cap)
                print("    客户端 network_capture.bin 里 CustomByteBlock(rx) = %d 条" % n_rx)
            except OSError as e:
                print("    读不了 network_capture.bin: %s" % e)
        if n_ok == 0:
            print("    → 这个会话里客户端一次都没连上 MQTT（握手没成功），")
            print("      所以它收不到任何 CustomByteBlock，跟解包/解码无关。")
        elif n_rx == 0:
            print("    → 客户端连上了 MQTT、别的主题一直在收，但 CustomByteBlock 一条都没有。")
            print("      说明那段时间 0x0310 数据没有到达选手端 → 不是客户端解码的问题。")
        elif n_rx:
            print("    → 客户端确实收到了 CustomByteBlock，可以据此判断格式/解码。")


def _count_cbb_rx(cap_path):
    """只数 network_capture.bin 里 CustomByteBlock 的 rx 记录，不做完整解析。"""
    n = 0
    with open(cap_path, "rb") as fh:
        head = fh.read(16)
        if head[:8] != b"RBNCAP1\n":
            return 0
        while True:
            hdr = fh.read(12)
            if len(hdr) < 12 or hdr[:4] != b"REC2":
                break
            body_size = struct.unpack_from("<I", hdr, 4)[0]
            body = fh.read(body_size)
            if len(body) < body_size:
                break
            _t, _tr, direction, chlen, peerlen, _plen = struct.unpack_from("<QBBHHI", body, 0)
            channel = body[18:18 + chlen].decode("utf-8", "replace")
            if channel == "CustomByteBlock" and direction == 1:
                n += 1
    return n


def _mqtt_enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n > 0:
            b |= 0x80
        out.append(b)
        if n == 0:
            return bytes(out)


def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            return None
        buf += c
    return buf


def _mqtt_read(sock):
    hdr = _recv_exact(sock, 1)
    if hdr is None:
        return None, None
    ptype = hdr[0] >> 4
    mult, value = 1, 0
    while True:
        b = _recv_exact(sock, 1)
        if b is None:
            return None, None
        value += (b[0] & 0x7F) * mult
        if not (b[0] & 0x80):
            break
        mult *= 128
    data = _recv_exact(sock, value) if value else b""
    if data is None:
        return None, None
    return ptype, data


def cmd_probe(args):
    cid = str(args.robot).encode()
    print("=== MQTT 探针 ===")
    print("目标 %s:%d  clientID=%s  主题=%s" % (args.host, args.port, cid.decode(), args.topic))
    if args.host in ("127.0.0.1", "localhost"):
        print("⚠️  127.0.0.1 = 本机。只有本机跑着模拟裁判系统时，这里才有数据；")
        print("    比赛/实车联调要看的是 192.168.12.1（选手端）。两边的包不是同一份。")
    print("⚠️  同一个机器人 ID 只能有一个 MQTT 客户端。跑这个之前**先关掉 RoboGo**，")
    print("    否则要么你也收不到，要么把 RoboGo 顶下线。")
    print("")
    try:
        sock = socket.create_connection((args.host, args.port), timeout=8)
    except Exception as e:
        print("连不上 %s:%d —— %s" % (args.host, args.port, e))
        print("检查：网线插的是不是选手端那一侧的口？本机 IP 是不是裁判系统网段？防火墙？")
        return 1
    sock.settimeout(1.0)

    var = struct.pack("!H", 4) + b"MQTT" + bytes([4, 0x02]) + struct.pack("!H", 30)
    body = var + struct.pack("!H", len(cid)) + cid
    sock.sendall(b"\x10" + _mqtt_enc_len(len(body)) + body)
    ptype, data = _mqtt_read(sock)
    if ptype != 2 or not data:
        print("没有 CONNACK，ptype=%s" % ptype)
        return 1
    if data[1] != 0:
        print("CONNACK rc=%d（1=协议版本 2=clientID 非法 3=服务不可用 4=账号密码 5=未授权）" % data[1])
        print("clientID 必须是合法机器人 ID，例如 3 / 4 / 104")
        return 1
    print("[OK] 已连接")

    topic = args.topic.encode()
    sub = struct.pack("!H", 1) + struct.pack("!H", len(topic)) + topic + b"\x00"
    sock.sendall(b"\x82" + _mqtt_enc_len(len(sub)) + sub)
    ptype, _ = _mqtt_read(sock)
    print("[OK] 已订阅 %s" % args.topic if ptype == 9 else "[警告] SUBACK type=%s" % ptype)

    print("\n监听 %d 秒…\n" % args.seconds)
    start = time.time()
    n = 0
    sizes = {}
    all_pkts = []
    arrivals = []
    other_topics = {}
    next_report = start + 5.0
    while time.time() - start < args.seconds:
        try:
            ptype, data = _mqtt_read(sock)
        except socket.timeout:
            ptype = 0
        except Exception as e:
            print("[断开] %s" % e)
            break
        if ptype is None:
            print("[断开] 服务器关闭了连接")
            break
        if ptype == 3 and data and len(data) >= 2:
            tl = struct.unpack("!H", data[:2])[0]
            tp = data[2:2 + tl].decode("utf-8", "replace")
            body = data[2 + tl:]
            if tp != args.topic:
                other_topics[tp] = other_topics.get(tp, 0) + 1
            else:
                inner, note = _decode_cbb(body)
                sizes[len(inner)] = sizes.get(len(inner), 0) + 1
                pkts, _ = parse_payload_300(inner, args.style)
                all_pkts.extend(pkts)
                arrivals.append(time.time())
                n += 1
                if n <= 5 or n % 20 == 0:
                    print("  [%d] %dB → 内层 %s" % (
                        n, len(inner),
                        ",".join("seq=%d %s" % (p["seq"], p["magic"]) for p in pkts) or "无"))
        if time.time() >= next_report:
            print("--- %.0fs: CustomByteBlock %d 条 ---" % (time.time() - start, n))
            next_report += 5.0

    el = time.time() - start
    print("\n=== 探针结果（%.0f 秒）===" % el)
    print("  CustomByteBlock: %d 条 (%.2f 条/秒)" % (n, n / el if el else 0))
    if sizes:
        print("  负载长度分布: %s" % sizes)
    if other_topics:
        print("  同时收到的其它主题（说明网络/ID 没问题）: %s" % other_topics)
    if all_pkts:
        st = seq_stats([p["seq"] for p in all_pkts])
        print("  内层序号: 包=%d 唯一=%d 丢=%d 重复=%d" % (
            st["count"], st["unique"], st["lost"], max(0, st["count"] - st["unique"])))
        print("  缺口: %s" % (st["lost_list"] or "无"))
    if len(arrivals) >= 2:
        span = arrivals[-1] - arrivals[0]
        gaps = [b - a for a, b in zip(arrivals, arrivals[1:])]
        gaps_sorted = sorted(gaps)
        print("  到达时间: 首=%.1fs 末=%.1fs 跨度=%.1fs" % (
            arrivals[0] - start, arrivals[-1] - start, span))
        print("  到达间隔: 中位 %.2fs  最大 %.2fs  最小 %.3fs" % (
            gaps_sorted[len(gaps_sorted) // 2], max(gaps), min(gaps)))
        print("  （对照：send 用 --hz 1 每秒发 1 包，如果这里中位≈1s 说明基本没丢；")
        print("    中位明显大于 1s 说明一路在丢包）")
    print("")
    if n == 0:
        print("  ❌ 0 条 CustomByteBlock")
        if other_topics:
            print("     → 但收到了别的主题：网络和机器人 ID 都对，只是这个通道上没有数据。")
        print("     → 断点在 VT03→VT13→选手端 这一段（或 STM32 根本没发）。")
        print("     → 做『电脑灌包进 VT03』实验，把 STM32 排除掉。")
    elif sizes.get(309):
        print("  ⚠️ 收到 309 字节的包 → VT03 原样透传了 0x0310 帧，客户端会因为超过 300 字节丢掉。")
        print("     → 让 STM32 只发 300 字节 data（去掉 A5 外壳）。")
    else:
        print("  ✅ 有数据到 MQTT：VT03/VT13/选手端这一路在转发这 300 字节内容。")
        if all_pkts and any(p["magic"] == "RM26" for p in all_pkts):
            print("     但内层是 RM26 标记 = rm0310.py send 灌进去的测试包，不是相机视频。")
            print("     它只证明『VT03 会原样转发 300 字节』；真视频链路要另测（send 关掉、只跑相机）。")
        elif all_pkts and any(str(p["magic"]).startswith("H264") for p in all_pkts):
            print("     内层确实是 H.264 → 真视频已经到 MQTT 了。这时才轮到看客户端解码/显示。")
        print("     另外：probe 会顶掉同 ID 的客户端，所以这一轮里客户端是收不到的；")
        print("     要判断客户端，看下面『同一时间窗内客户端自己的证据』或关掉 probe 后看 network_capture.bin。")

    _report_robo_side(start, time.time())
    sock.close()
    return 0


# --------------------------------------------------------------------------
# 子命令：selftest
# --------------------------------------------------------------------------
def cmd_selftest(args):
    ok = 0
    fail = 0

    def check(name, cond, detail=""):
        nonlocal ok, fail
        if cond:
            ok += 1
            print("  [OK]   %s %s" % (name, detail))
        else:
            fail += 1
            print("  [FAIL] %s %s" % (name, detail))

    print("=== rm0310.py 自检（不需要任何硬件）===")

    # 1) CRC 表自洽：对同一数据，查表法和逐位法必须一致
    def crc16_bitwise(data):
        c = 0xFFFF
        for b in bytearray(data):
            c ^= b
            for _ in range(8):
                c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
        return c & 0xFFFF

    def crc8_bitwise(data):
        # 协议附录一的 CRC8 表是**反射式**（LSB 优先，poly 0x8C，init 0xFF）。
        # 这个函数就是用逐位算法复现同一张表，用来证明确实是这条多项式，
        # 而不是抄错表。表本身另与 STM32 video_forward.c 逐字节比对过（256/256 一致）。
        c = 0xFF
        for b in bytearray(data):
            c ^= b
            for _ in range(8):
                lsb = c & 1
                c >>= 1
                if lsb:
                    c ^= 0x8C
        return c

    sample = bytes(range(64))
    check("CRC16 查表 == 逐位", crc16(sample) == crc16_bitwise(sample),
          "0x%04x" % crc16(sample))
    check("CRC8 表 == 0x8C 反射多项式", crc8(sample) == crc8_bitwise(sample),
          "0x%02x" % crc8(sample))

    # 2) 309 字节帧往返
    f = make_frame_309(42, 7)
    check("帧长 309", len(f) == 309)
    check("帧头 0xA5", f[0] == 0xA5)
    check("data_length=300 LE", f[1] | (f[2] << 8) == 300)
    check("cmd=0x0310 LE", f[5] | (f[6] << 8) == 0x0310)
    check("CRC8 自洽", crc8(f[0:4]) == f[4])
    check("CRC16 自洽", crc16(f[0:307]) == (f[307] | (f[308] << 8)))
    frames, junk = parse_0x0310_stream(b"\x00" * 5 + f + f)
    check("流解析找到 2 帧、跳过 5 字节垃圾", len(frames) == 2 and junk == 5,
          "frames=%d junk=%d" % (len(frames), junk))
    check("解析出的两帧 CRC 都对", all(x["crc8_ok"] and x["crc16_ok"] for x in frames))

    # 3) 三种口径解析
    pkt = make_payload_300(99, 5)
    ps, notes = parse_payload_300(pkt, "canon")
    check("canon 解析出 1 个内层包", len(ps) == 1, str(notes))
    check("canon seq=99", ps and ps[0]["seq"] == 99)
    check("canon 识别 RM26 和心跳", ps and ps[0]["magic"] == "RM26" and ps[0]["beat"] == 5,
          str(ps[0]) if ps else "")
    check("canon 内层长度=298", ps and ps[0]["len"] == 298)

    ps, _ = parse_payload_300(make_direct_300(1234, 9), "direct")
    check("direct 解析 seq=1234", ps and ps[0]["seq"] == 1234)
    ps, _ = parse_payload_300(make_direct_300(1234, 9), "auto")
    check("auto 对裸序号包回退成功", ps and ps[0]["seq"] == 1234)

    ps, notes = parse_payload_300(f[7:307], "auto")
    check("frame 里的 data 能解出 seq=42", ps and ps[0]["seq"] == 42, str(notes))

    # 4) 真实 H.264 形状
    h264 = bytearray(300)
    struct.pack_into(">H", h264, 0, 298)
    struct.pack_into(">H", h264, 2, 500)
    h264[4:9] = b"\x00\x00\x00\x01\x65"
    ps, _ = parse_payload_300(bytes(h264), "canon")
    check("H.264 IDR 被识别", ps and ps[0]["magic"] == "H264 IDR", str(ps[0]) if ps else "")

    # 5) 非法长度
    bad = bytearray(300)
    struct.pack_into(">H", bad, 0, 299)
    ps, notes = parse_payload_300(bytes(bad), "canon")
    check("内层长度 299 被拒（>298）", not ps and any("298" in n for n in notes), str(notes))

    # 6) 序号统计
    st = seq_stats([1, 2, 3, 5, 6, 6, 7, 9])
    check("seq_stats 丢 2 个", st["lost"] == 2, str(st["lost_list"]))
    check("seq_stats 重复=6", 6 in st["dups"])
    st = seq_stats([65534, 65535, 0, 1])
    check("16 位回绕不算丢", st["lost"] == 0, str(st))

    # 7) CustomByteBlock protobuf 往返
    inner = make_payload_300(7, 1)
    body = bytes([0x0A]) + _mqtt_enc_len(len(inner)) + inner
    got, note = _decode_cbb(body)
    check("protobuf 解 data", got == inner, note)

    # 8) 造一个假的 RoboGo 抓包，验证 dump 能读
    def rec(t_us, transport, direction, chan, peer, payload):
        """与 logging.cpp build_record_body() 严格一致：
        u64 t_us | u8 transport | u8 dir | u16 chlen | u16 peerlen | u32 plen | channel | peer | payload
        """
        cb, pb = chan.encode(), peer.encode()
        b = struct.pack("<QBBHHI", t_us, transport, direction,
                        len(cb), len(pb), len(payload)) + cb + pb + payload
        return b"REC2" + struct.pack("<II", len(b), 0) + b

    import tempfile
    cap = bytearray(b"RBNCAP1\n" + struct.pack("<II", 2, 0))
    for i in range(6):
        data = make_payload_300(i, i // 3)
        cap += rec(1000000 * i, 1, 1, "CustomByteBlock", "192.168.12.1:3333", b"\x0a" + _mqtt_enc_len(len(data)) + data)
    cap += rec(1000000, 2, 1, "udp-video", "192.168.12.1:3334", b"x" * 40)
    tmp = os.path.join(tempfile.gettempdir(), "rm0310_selftest.bin")
    with open(tmp, "wb") as fh:
        fh.write(bytes(cap))
    print("\n  --- dump 假抓包 ---")
    rc = _dump_robo_capture(bytes(cap), argparse.Namespace(style="canon", max=20, verbose=False))
    check("dump 假抓包成功", rc == 0)
    try:
        os.remove(tmp)
    except OSError:
        pass

    print("\n=== 自检结果：%d 通过，%d 失败 ===" % (ok, fail))
    if fail:
        print("有失败项，先别去测硬件，把输出发给 CH。")
        return 1
    print("工具本身没问题，可以去拆链路了。")
    return 0


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="0x0310 第二视角链路分段隔离诊断工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="最常用的四条：\n"
               "  python rm0310.py selftest\n"
               "  python rm0310.py ports\n"
               "  python rm0310.py send COM5 --style alt --hz 1\n"
               "  python rm0310.py dump \"C:\\...\\logs\\<会话>\\network_capture.bin\"\n")
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("ports", help="列串口")
    p.set_defaults(func=cmd_ports)

    p = sub.add_parser("send", help="灌包（电脑扮演 STM32 / 图传发送端）")
    p.add_argument("port")
    p.add_argument("--baud", type=int, default=BAUD)
    p.add_argument("--hz", type=float, default=1.0, help="发包频率，默认 1Hz，排查时不要超过 10")
    p.add_argument("--seconds", type=float, default=60.0)
    p.add_argument("--count", type=int, default=0, help="发够这么多个就停")
    p.add_argument("--start-seq", type=int, default=0)
    p.add_argument("--style", default="alt", choices=STYLE_ORDER + ["direct", "alt"],
                   help="发什么内容，默认 alt（canon/frame 交替）")
    p.add_argument("--verbose", action="store_true")
    p.set_defaults(func=cmd_send)

    p = sub.add_parser("recv", help="收包（电脑扮演 VT13 后面的电脑 / 示波器替代品）")
    p.add_argument("port")
    p.add_argument("--baud", type=int, default=BAUD)
    p.add_argument("--seconds", type=float, default=30.0)
    p.add_argument("--save", default="", help="把收到的 300B 负载存成文件，方便再 dump")
    p.add_argument("--verbose", action="store_true")
    p.set_defaults(func=cmd_recv)

    p = sub.add_parser("dump", help="拆包（0x0310 字节流 / 300B 包文件 / JSONL / RoboGo 抓包）")
    p.add_argument("path")
    p.add_argument("--style", default="canon", choices=["canon", "direct", "auto"],
                   help="300B 里内层包的口径，默认 canon（我方实现）")
    p.add_argument("--wrap", type=int, default=0, help="按多少字节一个包切（默认 300）")
    p.add_argument("--frame", action="store_true", help="强制按 309B 帧流解析")
    p.add_argument("--jsonl", action="store_true", help="输入是 recv 生成的 jsonl（保留兼容）")
    p.add_argument("--max", type=int, default=40)
    p.add_argument("--verbose", action="store_true")
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser("gen", help="生成 300B/309B 包文件（给 PowerShell 脚本用）")
    p.add_argument("out")
    p.add_argument("--count", type=int, default=200)
    p.add_argument("--start-seq", type=int, default=0)
    p.add_argument("--style", default="canon", choices=STYLE_ORDER + ["direct", "alt"])
    p.set_defaults(func=cmd_gen)

    p = sub.add_parser("probe", help="直接连 MQTT 看 CustomByteBlock（会顶掉同 ID 的客户端）")
    p.add_argument("--robot", default="3", help="机器人 ID，默认 3")
    p.add_argument("--host", default="192.168.12.1")
    p.add_argument("--port", type=int, default=3333)
    p.add_argument("--topic", default="CustomByteBlock")
    p.add_argument("--seconds", type=float, default=60.0)
    p.add_argument("--style", default="auto", choices=["canon", "direct", "auto"])
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("selftest", help="自检（不用接硬件）")
    p.set_defaults(func=cmd_selftest)

    args = ap.parse_args()
    if not hasattr(args, "func"):
        ap.print_help()
        return 0
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
