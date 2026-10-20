#!/usr/bin/env python3
"""verify_frames.py —— 校验 0x0310 帧序列格式

用法:
    python3 verify_frames.py video_test_frames.bin
"""

import sys

CRC8_TAB = None
CRC16_TAB = None


def load_crc_tables(crc_cpp_path):
    """从 tools/crc.cpp 提取真实 CRC 表"""
    import re
    global CRC8_TAB, CRC16_TAB
    src = open(crc_cpp_path, encoding="utf-8").read()

    def extract(name):
        m = re.search(name + r'\[256\]\s*=\s*\{(.*?)\};', src, re.S)
        vals = re.findall(r'0x[0-9a-fA-F]+', m.group(1))
        return [int(v, 16) for v in vals]

    CRC8_TAB = extract("CRC8_TABLE")
    CRC16_TAB = extract("CRC16_TABLE")
    assert len(CRC8_TAB) == 256 and len(CRC16_TAB) == 256


def get_crc8(data):
    crc = 0xFF
    for b in data:
        crc = CRC8_TAB[crc ^ b]
    return crc


def get_crc16(data):
    crc = 0xFFFF
    for b in data:
        crc = (crc >> 8) ^ CRC16_TAB[(crc ^ b) & 0xFF]
    return crc


FRAME_SIZE = 309
HEADER = 7
DATA_LEN = 300


def verify(path):
    data = open(path, "rb").read()
    print(f"文件大小: {len(data)} 字节")

    if len(data) % FRAME_SIZE != 0:
        print(f"❌ 总字节数 {len(data)} 不是 {FRAME_SIZE} 的整数倍")
        return False

    n_frames = len(data) // FRAME_SIZE
    print(f"帧数: {n_frames}")
    print()

    errors = []
    stats = {
        "sof_ok": 0,
        "len_ok": 0,
        "cmd_ok": 0,
        "crc8_ok": 0,
        "crc16_ok": 0,
        "seq_values": [],
        "payload_nonzero": 0,
    }

    for i in range(n_frames):
        f = data[i * FRAME_SIZE:(i + 1) * FRAME_SIZE]

        # 1. 帧头
        if f[0] == 0xA5:
            stats["sof_ok"] += 1
        else:
            errors.append(f"帧 {i}: 帧头 {f[0]:02X} != A5")

        # 2. 长度字段（小端 300）
        data_len = f[1] | (f[2] << 8)
        if data_len == DATA_LEN:
            stats["len_ok"] += 1
        else:
            errors.append(f"帧 {i}: 长度 {data_len} != 300")

        # 3. cmd_id（小端 0x0310）
        cmd = f[5] | (f[6] << 8)
        if cmd == 0x0310:
            stats["cmd_ok"] += 1
        else:
            errors.append(f"帧 {i}: cmd {cmd:04X} != 0310")

        # 4. CRC8
        crc8_calc = get_crc8(f[:4])
        if f[4] == crc8_calc:
            stats["crc8_ok"] += 1
        else:
            errors.append(f"帧 {i}: CRC8 {f[4]:02X} != {crc8_calc:02X}")

        # 5. CRC16
        crc16_calc = get_crc16(f[:HEADER + DATA_LEN])
        crc16_recv = f[HEADER + DATA_LEN] | (f[HEADER + DATA_LEN + 1] << 8)
        if crc16_calc == crc16_recv:
            stats["crc16_ok"] += 1
        else:
            errors.append(f"帧 {i}: CRC16 {crc16_recv:04X} != {crc16_calc:04X}")

        # 6. seq（应该递增，0~255 循环）
        stats["seq_values"].append(f[3])

        # 7. payload 是否非空
        payload = f[HEADER:HEADER + DATA_LEN]
        if any(b != 0 for b in payload):
            stats["payload_nonzero"] += 1

    # ---- 汇总 ----
    print("=== 校验结果 ===")
    print(f"帧头 0xA5:     {stats['sof_ok']}/{n_frames}")
    print(f"长度 300:      {stats['len_ok']}/{n_frames}")
    print(f"cmd 0x0310:    {stats['cmd_ok']}/{n_frames}")
    print(f"CRC8:          {stats['crc8_ok']}/{n_frames}")
    print(f"CRC16:         {stats['crc16_ok']}/{n_frames}")
    print(f"载荷非空:      {stats['payload_nonzero']}/{n_frames}")

    # seq 连续性检查
    seqs = stats["seq_values"]
    if len(seqs) > 1:
        gaps = []
        for i in range(1, len(seqs)):
            expected = (seqs[i - 1] + 1) & 0xFF
            if seqs[i] != expected:
                gaps.append((i, seqs[i - 1], seqs[i], expected))
        if gaps:
            print(f"seq 不连续:    {len(gaps)} 处")
            for g in gaps[:5]:
                print(f"   帧 {g[0]}: {g[1]} -> {g[2]} (期望 {g[3]})")
        else:
            print("seq 连续:      ✅")

    print()
    if errors:
        print(f"❌ 发现 {len(errors)} 个错误:")
        for e in errors[:20]:
            print(f"   {e}")
        if len(errors) > 20:
            print(f"   ... 还有 {len(errors) - 20} 个")
        return False
    else:
        print("✅ 全部检查通过")
        return True


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python3 verify_frames.py <video_test_frames.bin> [crc.cpp 路径]")
        sys.exit(1)

    bin_path = sys.argv[1]
    crc_path = sys.argv[2] if len(sys.argv) > 2 else "tools/crc.cpp"

    try:
        load_crc_tables(crc_path)
    except Exception as e:
        print(f"❌ 无法加载 CRC 表: {e}")
        print(f"   请指定 tools/crc.cpp 的正确路径")
        sys.exit(1)

    ok = verify(bin_path)
    sys.exit(0 if ok else 1)
