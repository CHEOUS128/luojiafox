#!/usr/bin/env python3
"""make_fake_frame.py —— 生成合法的 0x0310 假帧序列（给电控做转发验证用）

用法:
    python3 tools/make_fake_frame.py /tmp/fake_0310.bin --count 200
    python3 tools/make_fake_frame.py /tmp/fake_0310.bin --count 200 --payload-size 500
"""

import argparse
import struct
import sys
import os

# 从 tools/crc.cpp 读真实 CRC 表（与 verify_frames.py 相同思路）
def load_crc_tables(crc_cpp_path):
    import re
    src = open(crc_cpp_path, encoding="utf-8").read()

    def extract(name):
        m = re.search(name + r'\[256\]\s*=\s*\{(.*?)\};', src, re.S)
        vals = re.findall(r'0x[0-9a-fA-F]+', m.group(1))
        return [int(v, 16) for v in vals]

    crc8 = extract("CRC8_TABLE")
    crc16 = extract("CRC16_TABLE")
    assert len(crc8) == 256 and len(crc16) == 256
    return crc8, crc16


def crc8_calc(tab, data):
    crc = 0xFF
    for b in data:
        crc = tab[crc ^ b]
    return crc


def crc16_calc(tab, data):
    crc = 0xFFFF
    for b in data:
        crc = (crc >> 8) ^ tab[(crc ^ b) & 0xFF]
    return crc


def make_frame(outer_seq, inner_seq, chunk, crc8_tab, crc16_tab):
    """把一段 H.264 片段按客户端规则封成 1 个 309 字节帧（1 个内层包）"""
    n = min(296, len(chunk))
    inner = struct.pack('>H', inner_seq) + chunk[:n]
    tp = struct.pack('>H', len(inner)) + inner
    tp += bytes(300 - len(tp))

    f = bytearray(309)
    f[0] = 0xA5
    f[1] = 300 & 0xFF
    f[2] = (300 >> 8) & 0xFF
    f[3] = outer_seq & 0xFF
    f[4] = crc8_calc(crc8_tab, f[:4])
    f[5] = 0x10
    f[6] = 0x03
    f[7:307] = tp
    c16 = crc16_calc(crc16_tab, f[:307])
    f[307] = c16 & 0xFF
    f[308] = (c16 >> 8) & 0xFF
    return bytes(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output", help="输出 .bin 路径")
    ap.add_argument("--count", type=int, default=200, help="帧数（默认 200，约 5 秒@40Hz）")
    ap.add_argument("--payload-size", type=int, default=500,
                    help="每帧模拟的 H.264 字节数（默认 500，会拆成多个 296B 包）")
    ap.add_argument("--crc", default=None, help="tools/crc.cpp 路径（默认自动找）")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    crc_path = args.crc or os.path.join(here, "crc.cpp")
    crc8_tab, crc16_tab = load_crc_tables(crc_path)

    outer_seq = 0
    inner_seq = 0
    frames = []
    # 简单可辨认的假数据，方便串口助手肉眼看
    data = bytes((i * 37 + 11) & 0xFF for i in range(args.payload_size))

    for _ in range(args.count):
        chunks = [data[i:i + 296] for i in range(0, len(data), 296)] or [b""]
        for ch in chunks:
            frames.append(make_frame(outer_seq, inner_seq, ch, crc8_tab, crc16_tab))
            outer_seq = (outer_seq + 1) & 0xFF
            inner_seq = (inner_seq + 1) & 0xFFFF

    with open(args.output, "wb") as fh:
        fh.write(b"".join(frames))

    print(f"已生成 {len(frames)} 帧 × 309 字节 = {len(frames) * 309} 字节 → {args.output}")
    print(f"外层 seq: 0~{(outer_seq - 1) & 0xFF}   内层 seq: 0~{(inner_seq - 1) & 0xFFFF}")
    print("校验: python3 tests/verify_frames.py " + args.output + " tools/crc.cpp")


if __name__ == "__main__":
    sys.exit(main())
