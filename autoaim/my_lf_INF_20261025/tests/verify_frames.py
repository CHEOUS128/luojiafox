#!/usr/bin/env python3
"""verify_frames.py —— 校验 0x0310 帧序列格式 + 内层解包

校验两层：
  外层：0x0310 帧（309 字节）—— 帧头/长度/cmd_id/CRC8/CRC16/seq
  内层：300 字节 transport packet —— 长度前缀/序号/填充/序号连续性

用法:
    python3 verify_frames.py video_test_frames.bin [tools/crc.cpp]
"""

import sys

CRC8_TAB = None
CRC16_TAB = None

FRAME_SIZE = 309
HEADER = 7
DATA_LEN = 300
TRANSPORT_PREFIX = 2
INNER_SEQ_BYTES = 2
MAX_INNER = DATA_LEN - TRANSPORT_PREFIX      # 298
MAX_PAYLOAD = MAX_INNER - INNER_SEQ_BYTES     # 296


def load_crc_tables(crc_cpp_path):
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


def parse_transport(tp):
    """解析 300 字节 transport packet，返回 (payload_bytes, inner_seqs, error)"""
    payload = bytearray()
    seqs = []
    off = 0
    while off + TRANSPORT_PREFIX <= len(tp):
        inner_len = (tp[off] << 8) | tp[off + 1]
        off += TRANSPORT_PREFIX
        if inner_len == 0:
            # 填充标记：之后必须全 0
            if any(b != 0 for b in tp[off:]):
                return None, None, "填充区有非零字节"
            break
        if inner_len > MAX_INNER:
            return None, None, f"内层包 {inner_len} 字节 > {MAX_INNER}"
        if off + inner_len > len(tp):
            return None, None, "内层包被截断"
        seq = (tp[off] << 8) | tp[off + 1]
        seqs.append(seq)
        payload += tp[off + INNER_SEQ_BYTES:off + inner_len]
        off += inner_len
    return bytes(payload), seqs, None


def verify(path):
    data = open(path, "rb").read()
    print(f"文件大小: {len(data)} 字节")

    if len(data) % FRAME_SIZE != 0:
        print(f"❌ 总字节数 {len(data)} 不是 {FRAME_SIZE} 的整数倍")
        return False

    n_frames = len(data) // FRAME_SIZE
    print(f"帧数: {n_frames}\n")

    errors = []
    cnt = {"sof": 0, "len": 0, "cmd": 0, "crc8": 0, "crc16": 0,
           "inner_ok": 0, "nonempty": 0, "tail": 0}

    all_inner_seqs = []
    h264_total = 0
    # 新增：统计每个包的「不满 296 字节的尾巴」数量。
    # 编码器每帧的最后一个 NAL 往往不到 296 字节，发送端会在本帧内立即发出
    # （补 0 到 300）。这属于正常现象，但占比应为 1 包/帧左右；
    # 如果每一个包都是短包，说明发送端仍在跨帧攒包（旧 bug）。
    short_packets = 0
    total_inner_packets = 0

    for i in range(n_frames):
        f = data[i * FRAME_SIZE:(i + 1) * FRAME_SIZE]

        if f[0] == 0xA5:
            cnt["sof"] += 1
        else:
            errors.append(f"帧 {i}: 帧头 {f[0]:02X} != A5")

        data_len = f[1] | (f[2] << 8)
        if data_len == DATA_LEN:
            cnt["len"] += 1
        else:
            errors.append(f"帧 {i}: 长度 {data_len} != 300")

        cmd = f[5] | (f[6] << 8)
        if cmd == 0x0310:
            cnt["cmd"] += 1
        else:
            errors.append(f"帧 {i}: cmd {cmd:04X} != 0310")

        if f[4] == get_crc8(f[:4]):
            cnt["crc8"] += 1
        else:
            errors.append(f"帧 {i}: CRC8 错误")

        crc16_calc = get_crc16(f[:HEADER + DATA_LEN])
        crc16_recv = f[HEADER + DATA_LEN] | (f[HEADER + DATA_LEN + 1] << 8)
        if crc16_calc == crc16_recv:
            cnt["crc16"] += 1
        else:
            errors.append(f"帧 {i}: CRC16 错误")

        # ---- 内层解包 ----
        tp = f[HEADER:HEADER + DATA_LEN]
        payload, seqs, err = parse_transport(tp)
        if err:
            errors.append(f"帧 {i}: 内层解析失败 - {err}")
        else:
            if payload:
                cnt["nonempty"] += 1
                h264_total += len(payload)
            if seqs:
                cnt["inner_ok"] += 1
                all_inner_seqs.extend(seqs)
                # 统计短包（内层载荷 < 296 字节，通常是每帧最后一个 NAL）
                off2 = 0
                while off2 + TRANSPORT_PREFIX <= len(tp):
                    n = (tp[off2] << 8) | tp[off2 + 1]
                    if n == 0:
                        break
                    total_inner_packets += 1
                    if n < MAX_INNER:
                        short_packets += 1
                    off2 += TRANSPORT_PREFIX + n

    print("=== 外层校验（0x0310 帧）===")
    print(f"帧头 0xA5:     {cnt['sof']}/{n_frames}")
    print(f"长度 300:      {cnt['len']}/{n_frames}")
    print(f"cmd 0x0310:    {cnt['cmd']}/{n_frames}")
    print(f"CRC8:          {cnt['crc8']}/{n_frames}")
    print(f"CRC16:         {cnt['crc16']}/{n_frames}")

    print("\n=== 内层校验（transport packet）===")
    print(f"含有效内层包:  {cnt['inner_ok']}/{n_frames}")
    print(f"载荷非空:      {cnt['nonempty']}/{n_frames}")
    print(f"H.264 总字节:  {h264_total}")
    if total_inner_packets:
        pct = short_packets * 100.0 / total_inner_packets
        print(f"短包(<296B):   {short_packets}/{total_inner_packets} ({pct:.1f}%)  "
              "(每帧 1 个左右属正常；几乎全是短包说明还在跨帧攒包)")

    if all_inner_seqs:
        gaps = []
        for i in range(1, len(all_inner_seqs)):
            expected = (all_inner_seqs[i - 1] + 1) & 0xFFFF
            if all_inner_seqs[i] != expected:
                gaps.append((i, all_inner_seqs[i - 1], all_inner_seqs[i]))
        if gaps:
            print(f"内层 seq 不连续: {len(gaps)} 处（首个：{gaps[0]}）")
        else:
            print("内层 seq 连续: ✅")

    print()
    if errors:
        print(f"❌ 发现 {len(errors)} 个错误:")
        for e in errors[:20]:
            print(f"   {e}")
        if len(errors) > 20:
            print(f"   ... 还有 {len(errors) - 20} 个")
        return False
    print("✅ 全部检查通过")
    return True


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python3 verify_frames.py <video_test_frames.bin> [crc.cpp]")
        sys.exit(1)
    bin_path = sys.argv[1]
    crc_path = sys.argv[2] if len(sys.argv) > 2 else "tools/crc.cpp"
    try:
        load_crc_tables(crc_path)
    except Exception as e:
        print(f"❌ 无法加载 CRC 表: {e}")
        sys.exit(1)
    sys.exit(0 if verify(bin_path) else 1)
