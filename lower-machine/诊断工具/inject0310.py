#!/usr/bin/env python3
# -*- coding: gbk -*-
"""inject0310.py —— 不用装任何东西，往 VT03 灌 0x0310 视频包。

为什么单独写一个：这台电脑的 python 是微软商店的占位程序（跑什么都不输出），
装不了 pip / pyserial。本脚本只用 Windows 自带的系统调用（crosstalk 到 kernel32）
直接开 COM 口，所以**只要有能跑的 python 就行**。

如果 python 也跑不动，用清单文件 inject0310.bat（PowerShell 版）。

用法（在"诊断工具"目录里）:
    python inject0310.py COM3                 # 默认 5 包/秒，跑 60 秒
    python inject0310.py COM3 --hz 1 --seconds 30
    python inject0310.py ports                # 只列串口，不发包

接线（工具会再提醒一次）:
    PC 的 TX  ->  VT03 的 RX
    PC 的 GND ->  VT03 的 GND
    STM32 的 PG14 先拔掉（两个发送端不能顶在一起）
    VT03 用他自己的 12V 供电
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

BAUD = 921600
SOF = 0xA5
CMD_ID = 0x0310
VIDEO_DATA_BYTES = 300
VIDEO_FRAME_BYTES = 309
INNER_CANON_LEN = 298
RM_MAGIC = b"RM26"

# ---------------------------- CRC 表（与 STM32 / 上位机一致） ----------------------------
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


def make_payload_300(seq, beat):
    """和 Jetson 的 video_sender.cpp 完全一致的 300 字节 data：
       [2B 内层长度 BE = 298][2B 内层序号 BE]['RM26'][4B 心跳][288B 0]"""
    pkt = bytearray(VIDEO_DATA_BYTES)
    struct.pack_into(">H", pkt, 0, INNER_CANON_LEN)
    struct.pack_into(">H", pkt, 2, seq & 0xFFFF)
    pkt[4:8] = RM_MAGIC
    struct.pack_into(">I", pkt, 8, beat & 0xFFFFFFFF)
    return bytes(pkt)


def make_frame_309(seq, beat):
    """309 字节 0x0310 整帧 = STM32 从 PG14 实际发出去的形态"""
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


# ---------------------------- 串口（只用 kernel32） ----------------------------
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
PURGE_TXABORT = 0x0001
PURGE_RXABORT = 0x0002
PURGE_TXCLEAR = 0x0004
PURGE_RXCLEAR = 0x0008


class DCB(ctypes.Structure):
    _fields_ = [
        ("DCBlength", wt.DWORD),
        ("BaudRate", wt.DWORD),
        ("flags", wt.DWORD),          # fBinary / fParity / fDtrControl / fRtsControl ... 打包位域
        ("wReserved", wt.WORD),
        ("XonLim", wt.WORD),
        ("XoffLim", wt.WORD),
        ("ByteSize", wt.BYTE),
        ("Parity", wt.BYTE),
        ("StopBits", wt.BYTE),
        ("XonChar", ctypes.c_char),
        ("XoffChar", ctypes.c_char),
        ("ErrorChar", ctypes.c_char),
        ("EofChar", ctypes.c_char),
        ("EvtChar", ctypes.c_char),
        ("wReserved1", wt.WORD),
    ]


class COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [
        ("ReadIntervalTimeout", wt.DWORD),
        ("ReadTotalTimeoutMultiplier", wt.DWORD),
        ("ReadTotalTimeoutConstant", wt.DWORD),
        ("WriteTotalTimeoutMultiplier", wt.DWORD),
        ("WriteTotalTimeoutConstant", wt.DWORD),
    ]


def build_dcb(baud):
    d = DCB()
    d.DCBlength = ctypes.sizeof(DCB)
    d.BaudRate = baud
    # fBinary=1(bit0) | fDtrControl=1(bit4) | fRtsControl=1(bit12)
    d.flags = 0x0001 | (1 << 4) | (1 << 12)
    d.ByteSize = 8
    d.Parity = 0        # NOPARITY
    d.StopBits = 0      # ONESTOPBIT
    return d


def open_port(port, baud):
    if not port.upper().startswith("\\\\.\\"):
        port = "\\\\.\\" + port
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = wt.HANDLE
    k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p,
                                wt.DWORD, wt.DWORD, wt.HANDLE]
    k32.SetCommState.argtypes = [wt.HANDLE, ctypes.POINTER(DCB)]
    k32.SetCommTimeouts.argtypes = [wt.HANDLE, ctypes.POINTER(COMMTIMEOUTS)]
    k32.PurgeComm.argtypes = [wt.HANDLE, wt.DWORD]
    k32.WriteFile.restype = wt.BOOL
    k32.WriteFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD,
                              ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
    k32.FlushFileBuffers.argtypes = [wt.HANDLE]
    k32.CloseHandle.argtypes = [wt.HANDLE]

    h = k32.CreateFileW(port, GENERIC_READ | GENERIC_WRITE, 0, None,
                        OPEN_EXISTING, 0, None)
    if h is None or h == 0xFFFFFFFFFFFFFFFF:
        err = ctypes.get_last_error()
        raise OSError("打开 %s 失败（GetLastError=%d）。"
                      "5=被别的程序占用；2=串口号不存在；1167=设备已拔出" % (port, err))
    dcb = build_dcb(baud)
    if not k32.SetCommState(h, ctypes.byref(dcb)):
        raise OSError("SetCommState 失败（GetLastError=%d）" % ctypes.get_last_error())
    to = COMMTIMEOUTS(0xFFFFFFFF, 0, 0, 0, 2000)
    k32.SetCommTimeouts(h, ctypes.byref(to))
    k32.PurgeComm(h, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR)
    return k32, h


def write_all(k32, h, data):
    buf = ctypes.create_string_buffer(data, len(data))
    written = wt.DWORD(0)
    ok = k32.WriteFile(h, ctypes.cast(buf, ctypes.c_void_p), len(data),
                       ctypes.byref(written), None)
    if not ok:
        raise OSError("WriteFile 失败（GetLastError=%d）" % ctypes.get_last_error())
    return written.value


def close_port(k32, h):
    try:
        k32.FlushFileBuffers(h)
    except Exception:
        pass
    k32.CloseHandle(h)


def list_ports():
    """用系统注册表列串口，避免依赖 pyserial。"""
    ports = []
    try:
        import winreg
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                             r"HARDWARE\DEVICEMAP\SERIALCOMM")
        i = 0
        while True:
            try:
                name, value, _ = winreg.EnumValue(key, i)
            except OSError:
                break
            ports.append((value, name))
            i += 1
    except Exception as exc:
        print("读注册表列串口失败：%s" % exc)
    if not ports:
        print("没找到任何串口。检查 CH340 插好没有、驱动装没装（设备管理器里有没有 COM 口）。")
    for value, name in ports:
        print("  %-8s  <- %s" % (value, name))
    return ports


def main():
    ap = argparse.ArgumentParser(description="往 VT03 灌 0x0310 视频包（不用装 pyserial）")
    ap.add_argument("port", help="串口号，例如 COM3；写 ports 只列口不发包")
    ap.add_argument("--hz", type=float, default=5.0, help="每秒发几包，默认 5")
    ap.add_argument("--seconds", type=float, default=60.0, help="跑多少秒，默认 60")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--start-seq", type=int, default=0)
    args = ap.parse_args()

    if args.port.lower() == "ports":
        print("=== 本机串口 ===")
        list_ports()
        return 0

    print("=== 灌包：往 %s 发 309 字节 0x0310 整帧 ===" % args.port)
    print("波特率 %d 8N1，%g 包/秒，跑 %g 秒，共约 %d 包"
          % (args.baud, args.hz, args.seconds, int(args.hz * args.seconds)))
    print("")
    print("接线确认：")
    print("  PC 的 TX  ->  VT03 的 RX")
    print("  PC 的 GND ->  VT03 的 GND")
    print("  STM32 的 PG14 已拔掉（两个发送端不能顶在一起）")
    print("  VT03 用自己的 12V 供电")
    print("")

    k32, h = open_port(args.port, args.baud)
    period = 1.0 / max(args.hz, 0.1)
    seq = args.start_seq & 0xFFFF
    sent = 0
    t0 = time.time()
    next_tick = t0
    next_report = t0 + 2.0
    try:
        while time.time() - t0 < args.seconds:
            now = time.time()
            if now < next_tick:
                time.sleep(min(next_tick - now, 0.02))
                continue
            frame = make_frame_309(seq, sent)
            write_all(k32, h, frame)
            sent += 1
            seq = (seq + 1) & 0xFFFF
            next_tick += period
            if next_tick < now - period:      # 掉队了就别追
                next_tick = now + period
            if time.time() >= next_report:
                next_report += 2.0
                print("  已发 %d 包（%.1f 秒，实际 %.1f 包/秒）"
                      % (sent, time.time() - t0, sent / max(time.time() - t0, 0.001)))
    except KeyboardInterrupt:
        print("\n  手动中断")
    finally:
        close_port(k32, h)

    elapsed = time.time() - t0
    print("")
    print("=== 发完了：%d 包，%.1f 秒，平均 %.1f 包/秒 ===" % (sent, elapsed, sent / max(elapsed, 0.001)))
    print("这时应该关掉 RoboGo，然后 dump 它这一场的抓包，看 CustomByteBlock 有几条。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
