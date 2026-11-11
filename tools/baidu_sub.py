#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
百度云 IoT Core MQTT 订阅工具（纯 socket，不依赖 paho-mqtt）

用法:
    python tools/baidu_sub.py                          # 订阅 telemetry
    python tools/baidu_sub.py --topic $iot/stm32_gw_01/user/commands
    python tools/baidu_sub.py --all                    # 订阅 telemetry+commands+ota+acks
    python tools/baidu_sub.py --raw                    # 原始 hex 显示（不解析 JSON）
    python tools/baidu_sub.py --host xxx.iot.gz.baidubce.com --port 1883

默认订阅: $iot/stm32_gw_01/user/telemetry

⚠️ 重要警告:
    百度云同一设备身份只允许一个 MQTT 连接！
    本脚本用 stm32_gw_01 身份连接，会把板子踢下线。
    - 板子被踢后不再发遥测，PC 收不到数据
    - 想看板子发的数据，请用 --all 订阅所有 topic，板子重连的间隙可能抓到数据
    - 最可靠的方式：停板子 → PC 连 → 用 baidu_pub.py 往 telemetry topic 发测试消息 → PC 接收验证

⚠️ 三元组是真实凭据，git commit 前必须改回占位符！
"""
import sys
import socket
import struct
import hashlib
import json
import time
import datetime

# ===== 设备三元组（用 pc_observer 身份，不影响板子的 stm32_gw_01 连接）=====
# ⚠️ git commit 前改成 "YOUR_DEVICE_SECRET" 占位符
IOTCORE_ID    = "aeklwww"
DEVICE_KEY    = "pc_observer"
DEVICE_SECRET = "YOUR_DEVICE_SECRET"    # 占位符，测试时替换为真实密钥
REGION        = "gz"

HOST = "{}.iot.{}.baidubce.com".format(IOTCORE_ID, REGION)
PORT = 1883

# 默认订阅的 topic
TOPICS = {
    "telemetry": "$iot/{}/user/telemetry".format(DEVICE_KEY),
    "commands":  "$iot/{}/user/commands".format(DEVICE_KEY),
    "acks":      "$iot/{}/user/acks".format(DEVICE_KEY),
    "ota":       "$iot/{}/user/ota".format(DEVICE_KEY),
}


# ==================== MQTT 协议编解码 ====================

def encode_remaining_length(n):
    """MQTT 变长编码"""
    buf = b""
    while True:
        b = n & 0x7F
        n >>= 7
        if n > 0:
            b |= 0x80
        buf += bytes([b])
        if n == 0:
            break
    return buf


def decode_remaining_length(sock_rxbuf, pos):
    """解码变长长度，返回 (length, new_pos)"""
    multiplier = 1
    value = 0
    while True:
        if pos >= len(sock_rxbuf):
            return None, pos
        b = sock_rxbuf[pos]
        pos += 1
        value += (b & 0x7F) * multiplier
        if (b & 0x80) == 0:
            break
        multiplier *= 128    # MQTT 规范：每字节 7 位，权重 128 的幂
    return value, pos


def encode_string(s):
    """MQTT UTF-8 字符串：2字节长度 + 内容"""
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


def build_connect():
    """构造 MQTT 3.1.1 CONNECT 包（不带 LWT，CleanSession=1）"""
    ts = 0    # 百度云允许 ts=0，跳过时间校验
    username = "thingidp@{}|{}|{}|SHA256".format(IOTCORE_ID, DEVICE_KEY, ts)
    sign_str = "{}&{}&SHA256{}".format(DEVICE_KEY, ts, DEVICE_SECRET)
    password = hashlib.sha256(sign_str.encode("utf-8")).hexdigest()

    # 可变头
    vh = struct.pack(">H", 4) + b"MQTT"    # protocol name
    vh += b"\x04"                            # protocol level (3.1.1)
    vh += b"\xC2"                            # flags: CleanSession(0x02) | UserName(0x80) | Password(0x40)
    vh += struct.pack(">H", 60)             # keepalive 60s

    # 载荷
    payload = encode_string(DEVICE_KEY)     # ClientID
    payload += encode_string(username)      # Username
    payload += encode_string(password)      # Password

    # 固定头
    remaining = len(vh) + len(payload)
    return b"\x10" + encode_remaining_length(remaining) + vh + payload


def build_subscribe(topics, qos=0):
    """构造 SUBSCRIBE 包，topics 是 topic 字符串列表"""
    pkt_id = 1
    vh = struct.pack(">H", pkt_id)
    payload = b""
    for topic in topics:
        payload += encode_string(topic)
        payload += bytes([qos])
    remaining = len(vh) + len(payload)
    return b"\x82" + encode_remaining_length(remaining) + vh + payload


def build_disconnect():
    """构造 DISCONNECT 包"""
    return b"\xE0\x00"


def build_pingreq():
    """构造 PINGREQ 包"""
    return b"\xC0\x00"


def parse_publish(data):
    """
    从接收缓冲区解析一个 PUBLISH 包。
    返回 (topic, payload, qos, consumed_bytes) 或 (None, None, 0, 0) 如果数据不完整。
    """
    if len(data) < 2:
        return None, None, 0, 0

    pos = 0
    flags = data[pos]
    pkt_type = (flags >> 4) & 0x0F
    pos += 1

    if pkt_type != 3:    # 不是 PUBLISH
        return None, None, 0, 0

    qos = (flags >> 1) & 0x03

    remaining, pos = decode_remaining_length(data, pos)
    if remaining is None:
        return None, None, 0, 0

    total_len = pos + remaining
    if len(data) < total_len:
        return None, None, 0, 0    # 数据不完整

    # topic
    if pos + 2 > len(data):
        return None, None, 0, 0
    topic_len = struct.unpack(">H", data[pos:pos+2])[0]
    pos += 2
    if pos + topic_len > len(data):
        return None, None, 0, 0
    topic = data[pos:pos+topic_len].decode("utf-8", errors="replace")
    pos += topic_len

    # packet ID (QoS > 0 时有)
    if qos > 0:
        if pos + 2 > len(data):
            return None, None, 0, 0
        pos += 2    # 跳过 packet ID

    # payload: 从 pos 到 total_len
    payload = data[pos:total_len]

    return topic, payload, qos, total_len


def parse_any_packet(data):
    """
    解析任意 MQTT 包，返回 (pkt_type, total_len) 或 (None, 0)。
    用于跳过非 PUBLISH 包（CONNACK/SUBACK/PINGRESP 等）。
    """
    if len(data) < 2:
        return None, 0
    pos = 0
    pkt_type = (data[pos] >> 4) & 0x0F
    pos += 1
    remaining, pos = decode_remaining_length(data, pos)
    if remaining is None:
        return None, 0
    total_len = pos + remaining
    if len(data) < total_len:
        return None, 0
    return pkt_type, total_len


# ==================== 显示工具 ====================

# ANSI 颜色
C_RESET = "\033[0m"
C_GREEN = "\033[32m"
C_CYAN  = "\033[36m"
C_YELL  = "\033[33m"
C_RED   = "\033[31m"
C_BOLD  = "\033[1m"
C_DIM   = "\033[2m"


def format_payload(topic, payload, raw=False):
    """格式化 payload 显示（直接 print）"""
    ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

    if raw:
        print("{}[{}] {}{}  len={} hex={}".format(
            C_DIM, ts, C_RESET, topic, len(payload), payload.hex()))
        return

    # 尝试解析 JSON
    try:
        text = payload.decode("utf-8")
    except:
        print("{}[{}] {}{}  (binary {} bytes)".format(
            C_DIM, ts, C_RESET, topic, len(payload)))
        return

    print("{}[{}] {}{}  len={}".format(C_DIM, ts, C_RESET, topic, len(payload)))

    try:
        obj = json.loads(text)
        print(json.dumps(obj, ensure_ascii=False, indent=2))
    except json.JSONDecodeError:
        print("  {}".format(text))


# ==================== 主逻辑 ====================

def do_subscribe(topics, raw=False, timeout=30, once=False):
    """连接百度云，订阅 topic，循环接收 PUBLISH"""
    print("{}══════════════════════════════════════════════════════════{}".format(C_CYAN, C_RESET))
    print("{} 百度云 IoT Core MQTT 订阅工具{}".format(C_BOLD, C_RESET))
    print("{}══════════════════════════════════════════════════════════{}".format(C_CYAN, C_RESET))
    print("{}Broker:{}  {}:{}".format(C_DIM, C_RESET, HOST, PORT))
    print("{}Device:{}  {}".format(C_DIM, C_RESET, DEVICE_KEY))
    print("{}Topics:{} {}".format(C_DIM, C_RESET, ", ".join(topics)))
    print("{}Mode:{}    {}{}".format(C_DIM, C_RESET, "raw hex" if raw else "JSON pretty", " (once)" if once else ""))
    print("{}Timeout:{} {}s".format(C_DIM, C_RESET, timeout))
    print("{}══════════════════════════════════════════════════════════{}".format(C_CYAN, C_RESET))

    # TCP 连接
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    try:
        print("\n{}[TCP]{} connecting...".format(C_DIM, C_RESET), end="", flush=True)
        sock.connect((HOST, PORT))
        print(" {}OK{}".format(C_GREEN, C_RESET))
    except Exception as e:
        print(" {}FAIL{} ({})".format(C_RED, C_RESET, e))
        return

    try:
        # CONNECT
        connect_pkt = build_connect()
        sock.sendall(connect_pkt)
        print("{}[MQTT]{} CONNECT sent ({} bytes)".format(C_DIM, C_RESET, len(connect_pkt)))

        # 收 CONNACK
        sock.settimeout(5)
        data = sock.recv(1024)
        if len(data) >= 4 and data[0] == 0x20:
            rc = data[3]
            if rc == 0:
                print("{}[MQTT]{} CONNACK rc=0 {}Connected!{}".format(C_DIM, C_RESET, C_GREEN, C_RESET))
            else:
                rc_meaning = {
                    1: "协议版本不支持",
                    2: "Identifier Rejected",
                    3: "Server Unavailable",
                    4: "Bad Username/Password",
                    5: "Not Authorized",
                }
                print("{}[MQTT]{} CONNACK rc={} {}{}{}".format(
                    C_DIM, C_RESET, rc, C_RED,
                    rc_meaning.get(rc, "unknown"), C_RESET))
                return
        else:
            print("{}[MQTT]{} unexpected response: {}".format(
                C_DIM, C_RESET, data.hex() if data else "empty"))
            return

        # SUBSCRIBE
        sub_pkt = build_subscribe(topics, qos=0)
        sock.sendall(sub_pkt)
        print("{}[MQTT]{} SUBSCRIBE sent ({} topics)".format(C_DIM, C_RESET, len(topics)))

        # 收 SUBACK
        sock.settimeout(5)
        data = sock.recv(1024)
        if len(data) >= 3 and data[0] == 0x90:
            granted = list(data[4:]) if len(data) > 4 else []
            print("{}[MQTT]{} SUBACK OK (granted qos={})".format(C_DIM, C_RESET, granted))
        else:
            print("{}[MQTT]{} SUBACK unexpected: {}".format(
                C_DIM, C_RESET, data.hex() if data else "empty"))

        # 循环接收
        print("\n{}─── 等待消息 (Ctrl+C 退出) ───{}\n".format(C_CYAN, C_RESET))
        sock.settimeout(1)
        rxbuf = b""
        msg_count = 0
        last_ping = time.time()
        start_time = time.time()
        done = False

        while True:
            if done:
                break

            elapsed = time.time() - start_time
            if timeout > 0 and elapsed > timeout:
                print("\n{}[INFO]{} 超时 {}s，自动退出".format(C_DIM, C_RESET, timeout))
                break

            # PINGREQ (keepalive)
            if time.time() - last_ping > 30:
                sock.sendall(build_pingreq())
                last_ping = time.time()

            # 读数据
            try:
                data = sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                print("{}[INFO]{} 连接被关闭".format(C_YELL, C_RESET))
                break
            rxbuf += data

            # 解析所有包
            while rxbuf:
                pkt_type, pkt_len = parse_any_packet(rxbuf)
                if pkt_type is None or pkt_len == 0:
                    break    # 数据不完整，等更多

                if pkt_type == 3:    # PUBLISH
                    topic, payload, qos, consumed = parse_publish(rxbuf)
                    if topic is None:
                        break    # 数据不完整
                    msg_count += 1
                    format_payload(topic, payload, raw)
                    sys.stdout.flush()
                    rxbuf = rxbuf[consumed:]
                    if once and msg_count >= 1:
                        print("\n{}[INFO]{} --once 收到1条，退出".format(C_DIM, C_RESET))
                        done = True
                        break
                else:
                    # 跳过非 PUBLISH 包（CONNACK/SUBACK/PINGRESP/PUBACK 等）
                    type_names = {2: "CONNACK", 4: "PINGRESP", 9: "SUBACK",
                                  10: "UNSUBACK", 13: "PINGRESP"}
                    name = type_names.get(pkt_type, "type{}".format(pkt_type))
                    if pkt_type != 4 and pkt_type != 13:
                        print("{}[MQTT]{} {} ({} bytes)".format(
                            C_DIM, C_RESET, name, pkt_len))
                    rxbuf = rxbuf[pkt_len:]

        # 统计
        print("\n{}══════════════════════════════════════════════════════════{}".format(C_CYAN, C_RESET))
        print("{}统计:{} 收到 {} 条消息，运行 {:.1f}s".format(
            C_DIM, C_RESET, msg_count, time.time() - start_time))
        print("{}══════════════════════════════════════════════════════════{}".format(C_CYAN, C_RESET))

    except KeyboardInterrupt:
        print("\n{}[INFO]{} 用户中断".format(C_YELL, C_RESET))
    finally:
        # 发 DISCONNECT 优雅断开
        try:
            sock.sendall(build_disconnect())
        except:
            pass
        sock.close()
        print("{}[TCP]{} disconnected".format(C_DIM, C_RESET))


def main():
    args = sys.argv[1:]

    if "-h" in args or "--help" in args:
        print(__doc__)
        print("\n可用 topic:")
        for k, v in TOPICS.items():
            print("  {:12s} {}".format(k, v))
        return

    # 解析参数
    raw = "--raw" in args
    if raw:
        args = [a for a in args if a != "--raw"]

    all_topics = "--all" in args
    if all_topics:
        args = [a for a in args if a != "--all"]

    once = "--once" in args
    if once:
        args = [a for a in args if a != "--once"]

    timeout = 30
    if "--timeout" in args:
        i = args.index("--timeout")
        timeout = int(args[i + 1])
        args = args[:i] + args[i + 2:]

    # 确定订阅 topic
    if "--topic" in args:
        i = args.index("--topic")
        topics = [args[i + 1]]
        args = args[:i] + args[i + 2:]
    elif all_topics:
        topics = list(TOPICS.values())
    else:
        topics = [TOPICS["telemetry"]]

    do_subscribe(topics, raw=raw, timeout=timeout, once=once)


if __name__ == "__main__":
    main()
