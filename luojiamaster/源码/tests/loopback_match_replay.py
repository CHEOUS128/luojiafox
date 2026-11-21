"""Serve a 2027 match fixture to the Windows client's official login on this PC.

TCP MQTT 3.1.1 and UDP both use 127.0.0.1. No client application messages
are applied or forwarded. Pass --check to validate the inputs without networking.
"""

import argparse
import hashlib
import heapq
import socket
import struct
import subprocess
import threading
import time
import zlib
from pathlib import Path


HERE = Path(__file__).resolve().parent
HOST = "127.0.0.1"
CAPTURE_SHA = "80673f9373cb8a9e50fff48cc290a1b75c05ba1fa6f2a1c9c2b40980ef7b9554"
UDP = b"video-stream"
CBB = b"CustomByteBlock"


def recording(path):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != CAPTURE_SHA:
        raise ValueError("Use the embedded dual video sample from tests/fixtures.")
    lanes = {UDP: [], CBB: []}
    pos = 16
    while pos < len(data):
        magic, length, crc = struct.unpack_from("<4sII", data, pos)
        body = data[pos + 12:pos + 12 + length]
        pos += 12 + length
        if magic != b"REC2" or len(body) != length or zlib.crc32(body) != crc:
            raise ValueError("Damaged capture")
        us, transport, direction, nch, npeer, size = struct.unpack_from("<QBBHHI", body)
        topic = body[18:18+nch]
        packet = body[18+nch+npeer:]
        if direction != 1 or (transport, topic) not in {(2, UDP), (1, CBB)} or len(packet) != size:
            raise ValueError("Unexpected record")
        lanes[topic].append((us, topic, packet))
    # LocalSessionReplay sorts each lane by capture timestamp, with UDP first on ties.
    for packets in lanes.values():
        packets.sort(key=lambda p: p[0])
    merged = list(heapq.merge(lanes[UDP], lanes[CBB], key=lambda p: p[0]))
    first = merged[0][0]
    period_us = merged[-1][0] - first + 50_000
    return [(us - first, topic, data) for us, topic, data in merged], period_us


def match_messages(exe, role, seconds, competition):
    result = subprocess.run([str(exe), "--emit-fixture", str(role), str(seconds), competition],
                            check=True, capture_output=True, text=True, encoding="ascii", timeout=20)
    ticks = [[] for _ in range(seconds)]
    for line in result.stdout.splitlines():
        tick, topic, hex_payload = line.split(" ", 2)
        tick = int(tick)
        if not 0 <= tick < seconds:
            raise ValueError(f"Invalid fixture tick {tick}")
        ticks[tick].append((topic.encode("ascii"), bytes.fromhex(hex_payload)))
    if any(not tick for tick in ticks):
        raise ValueError("Fixture does not cover all requested seconds")
    return ticks


def mqtt_packet(header, payload):
    length = len(payload)
    prefix = bytearray([header])
    while length >= 128:
        prefix.append(128 | length % 128)
        length //= 128
    prefix.append(length)
    return bytes(prefix) + payload


def mqtt_read(conn):
    def exact(n):
        out = bytearray()
        while len(out) < n:
            chunk = conn.recv(n - len(out))
            if not chunk:
                raise EOFError()
            out.extend(chunk)
        return bytes(out)

    header = exact(1)[0]
    length = 0
    for digit in range(4):
        value = exact(1)[0]
        length |= (value & 127) << (7 * digit)
        if value < 128:
            if length > 65535:
                raise ValueError("MQTT input too large")
            return header, exact(length)
    raise ValueError("Invalid MQTT length")


def mqtt_string(data, i):
    n, = struct.unpack_from(">H", data, i)
    end = i + 2 + n
    if end > len(data):
        raise ValueError("Truncated MQTT string")
    return data[i+2:end], end


def handshake(conn, competition):
    header, body = mqtt_read(conn)
    if header != 0x10 or body[:7] != b"\x00\x04MQTT\x04":
        raise ValueError("Expected MQTT 3.1.1 CONNECT")
    role = int(mqtt_string(body, 10)[0])
    roles = (1, 3, 4, 101, 103, 104) if competition == "uc2027" else (1, 3, 101, 103)
    if role not in roles:
        raise ValueError(f"Use an operator role for {competition}")
    conn.sendall(mqtt_packet(0x20, bytes(2)))
    header, body = mqtt_read(conn)
    if header != 0x82:
        raise ValueError("Expected SUBSCRIBE")
    topics = []
    i = 2
    while i < len(body):
        topic, i = mqtt_string(body, i)
        if i >= len(body):
            raise ValueError("Missing subscription QoS")
        topics.append(topic)
        i += 1
    conn.sendall(mqtt_packet(0x90, body[:2] + bytes(len(topics))))
    return role, set(topics)


def run_session(conn, udp, events, period, args):
    role, subscriptions = handshake(conn, args.competition)
    ticks = match_messages(args.fixture_exe, role, args.seconds, args.competition)
    sent_topics = {topic for tick in ticks for topic, _ in tick} | {CBB}
    if not sent_topics <= subscriptions:
        raise ValueError(f"Missing official subscriptions: {sent_topics - subscriptions}")
    running = threading.Event()
    running.set()
    send_lock = threading.Lock()

    def send_mqtt(header, payload):
        with send_lock:
            conn.sendall(mqtt_packet(header, payload))

    def read_requests():
        try:
            while running.is_set():
                header, body = mqtt_read(conn)
                if header == 0xC0:
                    send_mqtt(0xD0, b"")
                elif header == 0xE0:
                    break
                elif (header & 0xF6) == 0x32:
                    # Transport acknowledgement only; discard application payload.
                    _, offset = mqtt_string(body, 0)
                    send_mqtt(0x40, body[offset:offset+2])
        except (ConnectionError, EOFError, OSError):
            pass
        finally:
            running.clear()

    conn.settimeout(None)
    reader = threading.Thread(target=read_requests, daemon=True)
    reader.start()
    start = time.monotonic()
    tick = index = epoch_us = 0
    counts = {UDP: 0, CBB: 0}
    print(f"CONNECTED role={role} topics={len(subscriptions)} start={time.time():.3f}", flush=True)
    try:
        while running.is_set():
            relative_us, video_topic, video_payload = events[index]
            video_us = epoch_us + relative_us
            next_us = min(tick * 1_000_000, video_us)
            if next_us >= args.seconds * 1_000_000:
                break
            delay = start + next_us * 1e-6 - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            if time.monotonic() - (start + next_us * 1e-6) > 0.2:
                raise RuntimeError("Playback lost its clock by over 200 ms; timing comparison stopped.")
            if next_us == tick * 1_000_000:
                # With --hold-state-at, stop producing later match telemetry but
                # continue streaming normal video. This is for manual screenshots.
                if args.hold_state_at is None or tick <= args.hold_state_at:
                    for topic, payload in ticks[tick]:
                        send_mqtt(0x30, struct.pack(">H", len(topic)) + topic + payload)
                if tick in (0, 9, 14, 16, 18, 20, 26, 39) or tick == args.hold_state_at:
                    print(f"tick={tick} udp={counts[UDP]} cbb={counts[CBB]}", flush=True)
                tick += 1
                continue
            second = video_us // 1_000_000
            if video_topic == UDP:
                if not 16 <= second < 18:
                    udp.sendto(video_payload, (HOST, args.video_port))
                    counts[UDP] += 1
            elif not 20 <= second < 26:
                send_mqtt(0x30, struct.pack(">H", len(CBB)) + CBB + video_payload)
                counts[CBB] += 1
            index += 1
            if index == len(events):
                index = 0
                epoch_us += period
    except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
        pass
    finally:
        running.clear()
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        reader.join(timeout=2)
        print(f"END udp={counts[UDP]} cbb={counts[CBB]} seconds={time.monotonic()-start:.2f}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, default=HERE / "fixtures" / "双路视频样本.bin")
    parser.add_argument("--fixture-exe", type=Path, default=HERE.parent / "build/release/match_session_test.exe")
    parser.add_argument("--competition", choices=("ul2027", "uc2027"), default="ul2027")
    parser.add_argument("--mqtt-port", type=int, default=59335)
    parser.add_argument("--video-port", type=int, default=59336)
    parser.add_argument("--seconds", type=int, default=180)
    parser.add_argument("--hold-state-at", type=int)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not (1 <= args.seconds <= 600 and 49152 <= args.mqtt_port <= 65535 and 49152 <= args.video_port <= 65535):
        parser.error("Use 1..600 seconds and ports 49152..65535.")
    if args.hold_state_at is not None and not 0 <= args.hold_state_at < args.seconds:
        parser.error("Hold tick must fall within replay.")
    events, period = recording(args.capture)
    for lane in (UDP, CBB):
        packets = [p for _, topic, p in events if topic == lane]
        print(f"{lane.decode()}: records={len(packets)} wire_sha256={hashlib.sha256(b''.join(packets)).hexdigest()}")
    if args.check:
        ticks = match_messages(args.fixture_exe, 3, args.seconds, args.competition)
        print(f"competition={args.competition} ticks={len(ticks)} messages={sum(map(len,ticks))} period_us={period}")
        return
    with socket.socket() as srv, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        srv.bind((HOST, args.mqtt_port))
        srv.listen(1)
        srv.settimeout(600)
        udp.bind((HOST, 0))
        print(f"READY {HOST}:{args.mqtt_port} UDP destination={HOST}:{args.video_port}", flush=True)
        conn, _ = srv.accept()
        with conn:
            conn.settimeout(10)
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            run_session(conn, udp, events, period, args)


if __name__ == "__main__":
    main()
