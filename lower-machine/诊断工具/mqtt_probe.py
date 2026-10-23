#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
最小 MQTT 订阅探针（标准库，无需 pip）
用法: python mqtt_probe.py <clientID> [host] [port]
  clientID 必须是合法机器人 ID（数字字符串）
  红方: 1英雄 2工程 3/4/5步兵 6空中 7哨兵
  蓝方: +100 (101/102/103/104/105/106/107)

示例:
  python mqtt_probe.py 3            # 用 3 号步兵（会踢掉同 ID 的客户端！）
  python mqtt_probe.py 104 192.168.12.1 3333

注意: 相同 clientID 会把已有的客户端踢下线。
"""
import socket, struct, sys, time

CLIENT_ID = (sys.argv[1] if len(sys.argv) > 1 else "3").encode()
HOST = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 3333
TOPIC = b"CustomByteBlock"

def enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n > 0:
            b |= 0x80
        out.append(b)
        if n == 0:
            return bytes(out)

def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            return None
        buf += c
    return buf

def read_packet(sock):
    hdr = recv_exact(sock, 1)
    if hdr is None:
        return None, None
    ptype = hdr[0] >> 4
    mult, value = 1, 0
    while True:
        b = recv_exact(sock, 1)
        if b is None:
            return None, None
        value += (b[0] & 0x7F) * mult
        if not (b[0] & 0x80):
            break
        mult *= 128
    data = recv_exact(sock, value) if value else b""
    if data is None:
        return None, None
    return ptype, data

def main():
    print(f"目标 {HOST}:{PORT}  clientID={CLIENT_ID.decode()}")
    sock = socket.create_connection((HOST, PORT), timeout=10)
    sock.settimeout(10)
    print("[OK] TCP 已连接")

    var = struct.pack("!H", 4) + b"MQTT" + bytes([4, 0x02]) + struct.pack("!H", 30)
    payload = struct.pack("!H", len(CLIENT_ID)) + CLIENT_ID
    body = var + payload
    sock.sendall(bytes([0x10]) + enc_len(len(body)) + body)

    ptype, data = read_packet(sock)
    if ptype != 2 or not data:
        print(f"[失败] 没有 CONNACK  ptype={ptype}")
        return 1
    rc = data[1]
    if rc != 0:
        reasons = {1:"协议版本不支持", 2:"clientID 被拒（不是合法机器人ID）",
                   3:"服务不可用", 4:"用户名密码错误", 5:"未授权"}
        print(f"[失败] CONNACK rc={rc}  {reasons.get(rc,'')}")
        print("       clientID 必须是数字机器人ID，如 3 / 4 / 104")
        return 1
    print("[OK] MQTT 握手成功")

    sub_body = struct.pack("!H", 1) + struct.pack("!H", len(TOPIC)) + TOPIC + b"\x00"
    sock.sendall(bytes([0x82]) + enc_len(len(sub_body)) + sub_body)
    ptype, _ = read_packet(sock)
    print(f"[OK] 已订阅 {TOPIC.decode()}" if ptype == 9 else f"[警告] SUBACK type={ptype}")

    print("\n等待 60 秒，有数据就打印...\n")
    sock.settimeout(1.0)
    start = time.time()
    count = total = 0
    last = start
    while time.time() - start < 60:
        try:
            ptype, data = read_packet(sock)
        except socket.timeout:
            ptype = 0
        except Exception as e:
            print(f"[断开] {e}")
            break
        if ptype is None:
            print("[断开] 服务器关闭连接")
            break
        if ptype == 3 and data and len(data) >= 2:
            tl = struct.unpack("!H", data[:2])[0]
            topic = data[2:2+tl]
            body = data[2+tl:]
            count += 1
            total += len(body)
            if count <= 20 or count % 50 == 0:
                print(f"  [{count}] {topic.decode(errors='replace')} len={len(body)}")
        if time.time() - last >= 10:
            print(f"--- 10s: {count} 条 / {total} 字节 ---")
            last = time.time()

    print(f"\n=== 60 秒结果 ===")
    print(f"  消息数: {count}")
    print(f"  字节数: {total}")
    print(f"  速率:   {count/60:.2f} 条/秒")
    print()
    if count == 0:
        print("  ❌ 0 条 → VT03 没有把自定义数据送进无线")
    else:
        print(f"  ✅ 有数据（{count}条）→ VT03 和选手端都正常，问题在客户端侧")
    return 0

if __name__ == "__main__":
    sys.exit(main())
