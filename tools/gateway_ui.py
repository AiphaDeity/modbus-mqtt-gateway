#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STM32 MQTT Gateway Web 控制台

功能：
  1. 实时查看设备上报到百度云的遥测数据（MQTT 订阅 pc_observer）
  2. 下发命令到设备（HTTP /pub 转发到 stm32_gw_01 的 commands topic）
  3. 历史消息回看 + 按从站分组的数据卡片

启动：
    python tools/gateway_ui.py
    python tools/gateway_ui.py --port 8080

浏览器打开 http://localhost:8080

架构：
  - MQTT 订阅线程：用 pc_observer 身份连百度云，订阅 $iot/pc_observer/user/#
    （规则引擎已把 stm32_gw_01 的遥测转发给 pc_observer，不会抢板子的 MQTT 连接）
  - HTTP Server (ThreadingHTTPServer)：
      GET /            → 返回 HTML 控制台页面
      GET /api/stream  → SSE 长连接，实时推送遥测消息
      GET /api/history → 最近 100 条历史消息 JSON
      GET /api/status  → MQTT 连接状态 + 统计
      POST /api/publish→ 下发命令（转发到百度云 HTTP /pub）

⚠️ 三元组是真实凭据，git commit 前必须改回占位符！
"""
import sys
import os
import socket
import struct
import hashlib
import json
import time
import threading
import datetime
import collections
import urllib.request
import urllib.parse
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ==================== 配置 ====================

# pc_observer：用于订阅遥测（规则引擎把 stm32_gw_01 的数据转发给它）
SUB_IOTCORE_ID = "aeklwww"
SUB_DEVICE_KEY = "pc_observer"
SUB_DEVICE_SECRET = "YOUR_DEVICE_SECRET"   # 占位符，测试时替换
SUB_REGION = "gz"

# stm32_gw_01：用于下发命令（HTTP /pub，不开 MQTT 长连接，不抢板子连接）
PUB_DEVICE_KEY = "stm32_gw_01"
PUB_DEVICE_SECRET = "YOUR_DEVICE_SECRET"   # 占位符，测试时替换

HOST = "{}.iot.{}.baidubce.com".format(SUB_IOTCORE_ID, SUB_REGION)
MQTT_PORT = 1883

# 订阅 topic（pc_observer 收到的转发数据）
# 注意：百度云 topic 权限在设备模板里配，通配符 # 需模板授权
# telemetry: 规则引擎转发板子遥测
# cmd_ack:   规则引擎转发板子命令回执（{"status":"ok","err":0}）
SUB_TOPICS = [
    "$iot/{}/user/telemetry".format(SUB_DEVICE_KEY),
    "$iot/{}/user/cmd_ack".format(SUB_DEVICE_KEY),
]
# 下发命令 topic（stm32_gw_01 监听的下行 topic）
PUB_TOPIC = "$iot/{}/user/commands".format(PUB_DEVICE_KEY)

WEB_PORT = 8080

# 历史消息上限
HISTORY_MAX = 200

# ==================== 全局状态 ====================

history = collections.deque(maxlen=HISTORY_MAX)   # [{ts, topic, payload, parsed}]
latest_by_slave = {}                                # {slave_id: {data, ts}}
sse_queues = []                                     # SSE 客户端的 Queue 列表
lock = threading.Lock()

mqtt_status = {
    "connected": False,
    "connect_time": None,
    "msg_count": 0,
    "last_msg_time": None,
    "error": None,
}

# ==================== MQTT 协议编解码（从 baidu_sub.py 复用）====================

def encode_remaining_length(n):
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


def decode_remaining_length(data, pos):
    multiplier = 1
    value = 0
    while True:
        if pos >= len(data):
            return None, pos
        b = data[pos]
        pos += 1
        value += (b & 0x7F) * multiplier
        if (b & 0x80) == 0:
            break
        multiplier *= 128
    return value, pos


def encode_string(s):
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


def build_connect(iotcore_id, device_key, device_secret):
    ts = 0
    username = "thingidp@{}|{}|{}|SHA256".format(iotcore_id, device_key, ts)
    sign_str = "{}&{}&SHA256{}".format(device_key, ts, device_secret)
    password = hashlib.sha256(sign_str.encode("utf-8")).hexdigest()

    vh = struct.pack(">H", 4) + b"MQTT"
    vh += b"\x04"
    vh += b"\xC2"
    vh += struct.pack(">H", 60)

    payload = encode_string(device_key)
    payload += encode_string(username)
    payload += encode_string(password)

    remaining = len(vh) + len(payload)
    return b"\x10" + encode_remaining_length(remaining) + vh + payload


def build_subscribe(topics, qos=0):
    pkt_id = 1
    vh = struct.pack(">H", pkt_id)
    payload = b""
    for topic in topics:
        payload += encode_string(topic)
        payload += bytes([qos])
    remaining = len(vh) + len(payload)
    return b"\x82" + encode_remaining_length(remaining) + vh + payload


def build_pingreq():
    return b"\xC0\x00"


def build_disconnect():
    return b"\xE0\x00"


def parse_any_packet(data):
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


def parse_publish(data):
    if len(data) < 2:
        return None, None, 0, 0
    pos = 0
    flags = data[pos]
    pkt_type = (flags >> 4) & 0x0F
    pos += 1
    if pkt_type != 3:
        return None, None, 0, 0
    qos = (flags >> 1) & 0x03

    remaining, pos = decode_remaining_length(data, pos)
    if remaining is None:
        return None, None, 0, 0
    total_len = pos + remaining
    if len(data) < total_len:
        return None, None, 0, 0

    if pos + 2 > len(data):
        return None, None, 0, 0
    topic_len = struct.unpack(">H", data[pos:pos+2])[0]
    pos += 2
    if pos + topic_len > len(data):
        return None, None, 0, 0
    topic = data[pos:pos+topic_len].decode("utf-8", errors="replace")
    pos += topic_len

    if qos > 0:
        if pos + 2 > len(data):
            return None, None, 0, 0
        pos += 2

    payload = data[pos:total_len]
    return topic, payload, qos, total_len


# ==================== HTTP /pub 下发（从 baidu_pub.py 复用）====================

def build_username_password(iotcore_id, device_key, device_secret, ts):
    username = "thingidp@{}|{}|{}|SHA256".format(iotcore_id, device_key, ts)
    sign_content = "{}&{}&SHA256{}".format(device_key, ts, device_secret)
    password = hashlib.sha256(sign_content.encode("utf-8")).hexdigest()
    return username, password


def get_token(username, password, lifespan=300):
    url = "http://{}/auth".format(HOST)
    body = json.dumps({
        "username": username,
        "password": password,
        "tokenLifeSpanInSeconds": lifespan
    }).encode("utf-8")
    req = urllib.request.Request(
        url, data=body,
        headers={"Content-Type": "application/json", "Accept": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            resp = json.loads(r.read().decode("utf-8"))
            return resp.get("token")
    except Exception:
        return None


def http_publish(topic, msg, qos=0):
    """通过百度云 HTTP /pub 接口下发消息，返回 (success, detail)"""
    ts = int(time.time())
    username, password = build_username_password(
        SUB_IOTCORE_ID, PUB_DEVICE_KEY, PUB_DEVICE_SECRET, ts)

    token = get_token(username, password)
    if not token:
        return False, "获取 token 失败"

    url = "http://{}/pub?topic={}&qos={}".format(
        HOST, urllib.parse.quote(topic, safe=""), qos)
    req = urllib.request.Request(
        url, data=msg.encode("utf-8") if isinstance(msg, str) else msg,
        headers={
            "token": token,
            "Content-Type": "application/octet-stream",
            "Accept": "application/json",
        }
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            resp = r.read().decode("utf-8", errors="replace")
            return True, "HTTP {} : {}".format(r.status, resp)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        return False, "HTTP {} : {}".format(e.code, body)
    except Exception as e:
        return False, str(e)


# ==================== 数据存储 + SSE 推送 ====================

def store_message(topic, payload_bytes):
    """存储消息并推送给所有 SSE 客户端"""
    ts_str = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

    # 尝试解析 JSON
    parsed = None
    try:
        text = payload_bytes.decode("utf-8")
        parsed = json.loads(text)
    except Exception:
        text = payload_bytes.hex()

    msg = {
        "ts": ts_str,
        "ts_epoch": time.time(),
        "topic": topic,
        "payload": text,
        "parsed": parsed,
        # 消息类型: telemetry=遥测数据, cmd_ack=命令回执, other=其他
        "msg_type": "cmd_ack" if "cmd_ack" in topic else ("telemetry" if "telemetry" in topic else "other"),
    }

    with lock:
        history.append(msg)
        mqtt_status["msg_count"] += 1
        mqtt_status["last_msg_time"] = ts_str

        # 如果是遥测数据，更新 latest_by_slave
        if parsed and isinstance(parsed, dict) and "slave" in parsed:
            sid = str(parsed["slave"])
            latest_by_slave[sid] = {
                "data": parsed,
                "ts": ts_str,
                "ts_epoch": time.time(),
            }

        # 推送给所有 SSE 客户端
        for q in sse_queues:
            try:
                q.put_nowait(msg)
            except Exception:
                pass


# ==================== MQTT 订阅线程 ====================

def mqtt_subscriber_thread():
    """后台线程：连接百度云 MQTT，订阅 pc_observer，持续接收遥测数据"""
    print("[MQTT] 订阅线程启动")

    while True:
        with lock:
            mqtt_status["connected"] = False
            mqtt_status["error"] = None

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)

        try:
            print("[MQTT] 连接 {}:{} ...".format(HOST, MQTT_PORT))
            sock.connect((HOST, MQTT_PORT))

            # CONNECT
            pkt = build_connect(SUB_IOTCORE_ID, SUB_DEVICE_KEY, SUB_DEVICE_SECRET)
            sock.sendall(pkt)

            # CONNACK
            sock.settimeout(5)
            data = sock.recv(1024)
            if len(data) >= 4 and data[0] == 0x20:
                rc = data[3]
                if rc != 0:
                    rc_map = {1: "协议版本不支持", 2: "Identifier Rejected",
                              3: "Server Unavailable", 4: "Bad Username/Password",
                              5: "Not Authorized"}
                    err = "CONNACK rc={} {}".format(rc, rc_map.get(rc, "unknown"))
                    print("[MQTT] {}".format(err))
                    with lock:
                        mqtt_status["error"] = err
                    sock.close()
                    print("[MQTT] 5秒后重连...")
                    time.sleep(5)
                    continue
            else:
                print("[MQTT] CONNACK 异常: {}".format(data.hex() if data else "empty"))
                sock.close()
                time.sleep(5)
                continue

            print("[MQTT] CONNECT 成功 (pc_observer)")

            # SUBSCRIBE（同时订阅 telemetry + cmd_ack）
            sub_pkt = build_subscribe(SUB_TOPICS, qos=0)
            sock.sendall(sub_pkt)
            print("[MQTT] SUBSCRIBE: {}".format(", ".join(SUB_TOPICS)))

            # SUBACK
            sock.settimeout(5)
            data = sock.recv(1024)
            sub_fail_topics = []
            if len(data) >= 3 and data[0] == 0x90:
                # SUBACK 格式: 0x90 + remaining_len + pkt_id(2B) + granted_qos_list
                granted = list(data[4:]) if len(data) > 4 else []
                # 检查每个 topic 的授权状态（0x80=拒绝）
                for i, g in enumerate(granted):
                    tname = SUB_TOPICS[i] if i < len(SUB_TOPICS) else "topic{}".format(i)
                    if g == 0x80:
                        print("[MQTT] ★订阅被拒绝(0x80): {} → 需在百度云设备模板配SUB权限".format(tname))
                        sub_fail_topics.append(tname)
                    else:
                        print("[MQTT] SUBACK OK: {} granted_qos={}".format(tname, g))
                # 如果所有 topic 都失败才断连；部分成功则继续
                if len(sub_fail_topics) == len(SUB_TOPICS):
                    err = "所有 topic 订阅被拒绝! granted={}".format(granted)
                    with lock:
                        mqtt_status["error"] = err
                    sock.close()
                    time.sleep(5)
                    continue
            else:
                print("[MQTT] SUBACK 异常: {}".format(data.hex() if data else "empty"))

            # 记录哪些 topic 订阅失败（供状态接口返回）
            with lock:
                mqtt_status["connected"] = True
                mqtt_status["connect_time"] = datetime.datetime.now().strftime("%H:%M:%S")
                mqtt_status["error"] = None
                if sub_fail_topics:
                    mqtt_status["sub_fail"] = sub_fail_topics
                else:
                    mqtt_status.pop("sub_fail", None)

            ok_topics = [t for t in SUB_TOPICS if t not in sub_fail_topics]
            print("[MQTT] 已连接，订阅成功: {} {}".format(
                ", ".join(ok_topics),
                "(部分失败: {})".format(", ".join(sub_fail_topics)) if sub_fail_topics else ""))

            # 循环接收
            sock.settimeout(1)
            rxbuf = b""
            last_ping = time.time()

            while True:
                # PINGREQ
                if time.time() - last_ping > 30:
                    try:
                        sock.sendall(build_pingreq())
                        last_ping = time.time()
                    except Exception:
                        break

                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    continue
                except Exception:
                    break

                if not data:
                    print("[MQTT] 连接被关闭")
                    break

                rxbuf += data

                # 解析所有包
                while rxbuf:
                    pkt_type, pkt_len = parse_any_packet(rxbuf)
                    if pkt_type is None or pkt_len == 0:
                        break

                    if pkt_type == 3:  # PUBLISH
                        topic, payload, qos, consumed = parse_publish(rxbuf)
                        if topic is None:
                            break
                        store_message(topic, payload)
                        rxbuf = rxbuf[consumed:]
                    else:
                        rxbuf = rxbuf[pkt_len:]

        except Exception as e:
            print("[MQTT] 异常: {}".format(e))
            with lock:
                mqtt_status["error"] = str(e)
        finally:
            try:
                sock.sendall(build_disconnect())
            except Exception:
                pass
            sock.close()
            with lock:
                mqtt_status["connected"] = False

        print("[MQTT] 5秒后重连...")
        time.sleep(5)


# ==================== HTTP Server ====================

HTML_PAGE = ""  # 在下方定义


class UIHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # 静默 HTTP 日志

    def _send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self._send_html(HTML_PAGE)
        elif self.path == "/api/status":
            with lock:
                slaves = {}
                for sid, info in latest_by_slave.items():
                    slaves[sid] = {
                        "ts": info["ts"],
                        "data": info["data"],
                    }
                status = {
                    "mqtt": dict(mqtt_status),
                    "msg_count": mqtt_status["msg_count"],
                    "slaves": slaves,
                    "history_size": len(history),
                }
            self._send_json(200, status)
        elif self.path == "/api/history":
            with lock:
                msgs = list(history)
            # 最新的在前
            msgs.reverse()
            self._send_json(200, msgs)
        elif self.path == "/api/stream":
            self._handle_sse()
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self):
        if self.path == "/api/publish":
            self._handle_publish()
        else:
            self._send_json(404, {"error": "not found"})

    def _send_html(self, html):
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _handle_publish(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length > 0 else b""

        try:
            body = json.loads(raw.decode("utf-8"))
        except Exception:
            self._send_json(400, {"ok": False, "error": "请求体不是有效 JSON"})
            return

        msg = body.get("msg", "")
        topic = body.get("topic", PUB_TOPIC)

        if not msg:
            self._send_json(400, {"ok": False, "error": "msg 为空"})
            return

        ok, detail = http_publish(topic, msg)
        self._send_json(200 if ok else 500, {"ok": ok, "detail": detail, "topic": topic, "msg": msg})

    def _handle_sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        # 注册 SSE 客户端：用 Queue（线程安全，有 put_nowait/get(timeout)）
        from queue import Queue, Empty
        q = Queue(maxsize=100)
        with lock:
            sse_queues.append(q)

        # 先发送最近 10 条历史
        with lock:
            recent = list(history)[-10:]
        for m in recent:
            try:
                data = json.dumps(m, ensure_ascii=False)
                self.wfile.write("data: {}\n\n".format(data).encode("utf-8"))
                self.wfile.flush()
            except Exception:
                pass

        # 持续推送
        try:
            while True:
                try:
                    m = q.get(timeout=5)
                    data = json.dumps(m, ensure_ascii=False)
                    self.wfile.write("data: {}\n\n".format(data).encode("utf-8"))
                    self.wfile.flush()
                except Empty:
                    # 超时无消息，发心跳保持连接
                    self.wfile.write(": heartbeat\n\n".encode("utf-8"))
                    self.wfile.flush()
        except Exception:
            pass
        finally:
            with lock:
                if q in sse_queues:
                    sse_queues.remove(q)


# ==================== 前端 HTML ====================

HTML_PAGE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>STM32 MQTT Gateway 控制台</title>
<style>
:root {
  --bg: #0d1117;
  --panel: #161b22;
  --border: #30363d;
  --text: #e6edf3;
  --dim: #8b949e;
  --green: #3fb950;
  --red: #f85149;
  --yellow: #d29922;
  --blue: #58a6ff;
  --accent: #1f6feb;
}
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
  font-family: -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif;
  background: var(--bg);
  color: var(--text);
  height: 100vh;
  overflow: hidden;
}
/* 顶部栏 */
.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 20px;
  height: 52px;
  background: var(--panel);
  border-bottom: 1px solid var(--border);
}
.header h1 {
  font-size: 16px;
  font-weight: 600;
}
.header .status-bar {
  display: flex;
  gap: 16px;
  align-items: center;
  font-size: 13px;
}
.status-pill {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 10px;
  border-radius: 6px;
  background: rgba(255,255,255,0.05);
}
.status-dot {
  width: 8px; height: 8px;
  border-radius: 50%;
  background: var(--dim);
}
.status-dot.online { background: var(--green); box-shadow: 0 0 6px var(--green); }
.status-dot.offline { background: var(--red); }
/* 主布局 */
.main {
  display: grid;
  grid-template-columns: 380px 1fr;
  height: calc(100vh - 52px);
}
/* 左侧：设备数据 */
.left-panel {
  border-right: 1px solid var(--border);
  overflow-y: auto;
  padding: 16px;
}
.left-panel h2 {
  font-size: 13px;
  color: var(--dim);
  text-transform: uppercase;
  letter-spacing: 1px;
  margin-bottom: 12px;
}
.slave-card {
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 14px;
  margin-bottom: 12px;
  transition: border-color 0.3s;
}
.slave-card.flash {
  border-color: var(--green);
  box-shadow: 0 0 8px rgba(63,185,80,0.3);
}
.slave-card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 10px;
}
.slave-card-header .slave-id {
  font-size: 15px;
  font-weight: 600;
}
.slave-card-header .slave-online {
  font-size: 12px;
  color: var(--green);
}
.slave-card-header .slave-offline {
  font-size: 12px;
  color: var(--red);
}
.slave-card-header .slave-ts {
  font-size: 11px;
  color: var(--dim);
}
.point-row {
  display: flex;
  justify-content: space-between;
  padding: 4px 0;
  font-size: 13px;
  border-top: 1px solid rgba(255,255,255,0.03);
}
.point-row:first-child { border-top: none; }
.point-name { color: var(--dim); }
.point-value { font-weight: 600; }
.point-unit { color: var(--dim); font-size: 11px; margin-left: 3px; }
.no-data {
  text-align: center;
  color: var(--dim);
  padding: 40px 0;
  font-size: 13px;
}
/* 右侧：消息流 + 命令面板 */
.right-panel {
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.right-top {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  padding: 16px;
}
.right-top h2 {
  font-size: 13px;
  color: var(--dim);
  text-transform: uppercase;
  letter-spacing: 1px;
  margin-bottom: 8px;
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.right-top h2 .clear-btn {
  font-size: 11px;
  color: var(--blue);
  cursor: pointer;
  background: none;
  border: none;
  padding: 2px 8px;
}
.msg-list {
  flex: 1;
  overflow-y: auto;
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 8px;
}
.msg-item {
  padding: 8px 10px;
  border-bottom: 1px solid rgba(255,255,255,0.03);
  font-size: 12px;
}
.msg-item:hover { background: rgba(255,255,255,0.02); }
.msg-meta {
  display: flex;
  gap: 10px;
  margin-bottom: 4px;
}
.msg-ts { color: var(--dim); font-family: monospace; }
.msg-item.msg-ack { border-left: 3px solid var(--green); padding-left: 8px; }
.msg-tag { font-size: 10px; padding: 1px 5px; border-radius: 3px; font-weight: 600; }
.msg-tag.ack { color: var(--green); background: rgba(63,185,80,0.15); }
.msg-topic { color: var(--blue); font-size: 11px; }
.msg-body {
  color: var(--text);
  font-family: "Cascadia Code", "Consolas", monospace;
  white-space: pre-wrap;
  word-break: break-all;
  font-size: 11px;
  line-height: 1.5;
  padding-left: 12px;
  border-left: 2px solid var(--border);
  margin-left: 2px;
}
/* 命令面板 */
.cmd-panel {
  border-top: 1px solid var(--border);
  padding: 16px;
  background: var(--panel);
}
.cmd-panel h2 {
  font-size: 13px;
  color: var(--dim);
  text-transform: uppercase;
  letter-spacing: 1px;
  margin-bottom: 10px;
}
.preset-row {
  display: flex;
  gap: 8px;
  margin-bottom: 10px;
  flex-wrap: wrap;
}
.preset-btn {
  padding: 6px 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: rgba(255,255,255,0.03);
  color: var(--text);
  font-size: 12px;
  cursor: pointer;
  transition: all 0.2s;
}
.preset-btn:hover {
  border-color: var(--accent);
  background: rgba(31,111,235,0.1);
}
.cmd-input-row {
  display: flex;
  gap: 8px;
}
.cmd-input {
  flex: 1;
  padding: 8px 12px;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text);
  font-family: "Cascadia Code", "Consolas", monospace;
  font-size: 12px;
  outline: none;
}
.cmd-input:focus { border-color: var(--accent); }
.send-btn {
  padding: 8px 20px;
  background: var(--accent);
  border: none;
  border-radius: 6px;
  color: white;
  font-size: 13px;
  font-weight: 600;
  cursor: pointer;
  transition: background 0.2s;
}
.send-btn:hover { background: #388bfd; }
.send-btn:disabled { background: var(--dim); cursor: not-allowed; }
.cmd-result {
  margin-top: 8px;
  font-size: 12px;
  font-family: monospace;
  padding: 6px 10px;
  border-radius: 4px;
  display: none;
}
.cmd-result.ok { display: block; color: var(--green); background: rgba(63,185,80,0.1); }
.cmd-result.err { display: block; color: var(--red); background: rgba(248,81,73,0.1); }

/* 配置变更生成器 */
.cfg-toggle {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: var(--blue);
  cursor: pointer;
  margin-bottom: 8px;
  user-select: none;
}
.cfg-toggle .arrow { transition: transform 0.2s; display: inline-block; }
.cfg-toggle.open .arrow { transform: rotate(90deg); }
.cfg-panel {
  display: none;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 12px;
  margin-bottom: 10px;
}
.cfg-panel.open { display: block; }
.cfg-row {
  display: flex;
  gap: 8px;
  margin-bottom: 8px;
  flex-wrap: wrap;
}
.cfg-field {
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.cfg-field label {
  font-size: 10px;
  color: var(--dim);
  text-transform: uppercase;
  letter-spacing: 0.5px;
}
.cfg-field input, .cfg-field select {
  padding: 4px 8px;
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 4px;
  color: var(--text);
  font-size: 12px;
  width: 110px;
  outline: none;
}
.cfg-field input:focus, .cfg-field select:focus { border-color: var(--accent); }
.cfg-field input[type="checkbox"] { width: auto; }
.cfg-checkbox {
  display: flex;
  align-items: center;
  gap: 4px;
  font-size: 12px;
  color: var(--text);
  margin-top: 18px;
}
.cfg-points {
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 8px;
  margin-bottom: 8px;
}
.cfg-point-item {
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 4px;
  padding: 8px;
  margin-bottom: 6px;
}
.cfg-point-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 6px;
  font-size: 11px;
  color: var(--dim);
}
.cfg-point-del {
  color: var(--red);
  cursor: pointer;
  background: none;
  border: none;
  font-size: 11px;
  padding: 0;
}
.cfg-btn-row {
  display: flex;
  gap: 8px;
  margin-top: 8px;
}
.cfg-btn {
  padding: 6px 14px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--panel);
  color: var(--text);
  font-size: 12px;
  cursor: pointer;
  transition: all 0.2s;
}
.cfg-btn:hover { border-color: var(--accent); color: var(--accent); }
.cfg-btn.primary { background: var(--accent); color: #fff; border-color: var(--accent); }
.cfg-btn.primary:hover { background: #388bfd; }
/* 滚动条 */
::-webkit-scrollbar { width: 6px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: var(--border); border-radius: 3px; }
::-webkit-scrollbar-thumb:hover { background: var(--dim); }
</style>
</head>
<body>

<div class="header">
  <h1>STM32 MQTT Gateway 控制台</h1>
  <div class="status-bar">
    <div class="status-pill">
      <div class="status-dot offline" id="mqtt-dot"></div>
      <span id="mqtt-status">MQTT 未连接</span>
    </div>
    <div class="status-pill">
      <span>消息: <strong id="msg-count">0</strong></span>
    </div>
    <div class="status-pill">
      <span>设备: <strong id="slave-count">0</strong></span>
    </div>
  </div>
</div>

<div class="main">
  <!-- 左侧：设备数据卡片 -->
  <div class="left-panel">
    <h2>设备遥测</h2>
    <div id="slave-list">
      <div class="no-data">等待数据...</div>
    </div>
  </div>

  <!-- 右侧：消息流 + 命令面板 -->
  <div class="right-panel">
    <div class="right-top">
      <h2>
        <span>消息流</span>
        <button class="clear-btn" onclick="clearMsgs()">清空</button>
      </h2>
      <div class="msg-list" id="msg-list">
        <div class="no-data">等待数据...</div>
      </div>
    </div>

    <div class="cmd-panel">
      <h2>下发命令</h2>
      <div class="preset-row" id="preset-row"></div>

      <!-- 配置变更生成器（可展开/收起） -->
      <div class="cfg-toggle" id="cfg-toggle" onclick="toggleCfgPanel()">
        <span class="arrow">▶</span>
        <span>配置变更生成器（新增/删除/修改从站）</span>
      </div>
      <div class="cfg-panel" id="cfg-panel">

        <!-- 操作类型 -->
        <div class="cfg-row">
          <div class="cfg-field">
            <label>操作</label>
            <select id="cfg-op">
              <option value="upsert">upsert（新增/更新）</option>
              <option value="delete">delete（删除）</option>
            </select>
          </div>
        </div>

        <!-- 从站参数（upsert 时显示） -->
        <div id="cfg-slave-fields">
          <div class="cfg-row">
            <div class="cfg-field">
              <label>从站地址</label>
              <input type="number" id="cfg-id" value="5" min="1" max="247">
            </div>
            <div class="cfg-field">
              <label>功能码</label>
              <select id="cfg-fc">
                <option value="1">FC01 读线圈</option>
                <option value="2">FC02 读离散</option>
                <option value="3" selected>FC03 读保持寄存器</option>
                <option value="4">FC04 读输入寄存器</option>
              </select>
            </div>
            <div class="cfg-field">
              <label>起始地址</label>
              <input type="number" id="cfg-reg" value="0" min="0">
            </div>
            <div class="cfg-field">
              <label>寄存器数</label>
              <input type="number" id="cfg-count" value="2" min="1" max="32">
            </div>
          </div>
          <div class="cfg-row">
            <div class="cfg-field">
              <label>采集周期(ms)</label>
              <input type="number" id="cfg-interval" value="2000" min="100" step="100">
            </div>
            <div class="cfg-field">
              <label>重试次数</label>
              <input type="number" id="cfg-retry" value="1" min="0">
            </div>
            <div class="cfg-checkbox">
              <input type="checkbox" id="cfg-enable" checked>
              <label for="cfg-enable" style="text-transform:none;letter-spacing:0;font-size:12px;color:var(--text)">启用</label>
            </div>
          </div>

          <!-- 点位表 -->
          <div style="font-size:11px;color:var(--dim);margin:8px 0 4px;">点位表</div>
          <div class="cfg-points" id="cfg-points-list"></div>
          <button class="cfg-btn" onclick="cfgAddPoint()">+ 添加点位</button>
        </div>

        <!-- 生成按钮 -->
        <div class="cfg-btn-row">
          <button class="cfg-btn primary" onclick="cfgGenerate()">生成并填入命令框</button>
          <button class="cfg-btn" onclick="cfgLoadTemplate('light')">模板:光照传感器</button>
          <button class="cfg-btn" onclick="cfgLoadTemplate('water')">模板:水浸传感器</button>
          <button class="cfg-btn" onclick="cfgLoadTemplate('temp')">模板:温湿度</button>
        </div>
      </div>

      <div class="cmd-input-row">
        <input type="text" class="cmd-input" id="cmd-input"
               placeholder='JSON 格式，如 {"slave":2,"func":6,"addr":0,"val":100}'
               value='{"slave":2,"func":6,"addr":0,"val":100}'>
        <button class="send-btn" id="send-btn" onclick="sendCmd()">发送</button>
      </div>
      <div class="cmd-result" id="cmd-result"></div>
    </div>
  </div>
</div>

<script>
// 预设命令
const PRESETS = [
  {label: "写线圈 ON",  cmd: '{"slave":1,"func":5,"addr":0,"val":1}'},
  {label: "写线圈 OFF", cmd: '{"slave":1,"func":5,"addr":0,"val":0}'},
  {label: "写寄存器 100", cmd: '{"slave":2,"func":6,"addr":0,"val":100}'},
  {label: "写寄存器 200", cmd: '{"slave":2,"func":6,"addr":0,"val":200}'},
  {label: "读保持寄存器", cmd: '{"slave":2,"func":3,"addr":0,"val":5}'},
  {label: "读输入寄存器", cmd: '{"slave":3,"func":4,"addr":0,"val":5}'},
  {label: "⚙ 新增光照传感器", cmd: '{"op":"upsert","slaves":[{"id":1,"fc":3,"reg":2,"count":2,"interval":2000,"enable":true,"retry":1,"points":[{"reg_index":0,"type":"uint32","scale":0.001,"name":"light","unit":"Lux"}]}]}'},
  {label: "⚙ 新增水浸传感器", cmd: '{"op":"upsert","slaves":[{"id":2,"fc":3,"reg":9088,"count":1,"interval":1000,"enable":true,"retry":2,"points":[{"reg_index":0,"type":"uint16","scale":1,"name":"water","unit":""}]}]}'},
  {label: "⚙ 删除从站4", cmd: '{"op":"delete","slaves":[{"id":4}]}'},
];

// 渲染预设按钮
const presetRow = document.getElementById('preset-row');
PRESETS.forEach(p => {
  const btn = document.createElement('button');
  btn.className = 'preset-btn';
  btn.textContent = p.label;
  btn.onclick = () => { document.getElementById('cmd-input').value = p.cmd; };
  presetRow.appendChild(btn);
});

// 清空消息
function clearMsgs() {
  document.getElementById('msg-list').innerHTML = '<div class="no-data">已清空</div>';
}

// ==================== 配置变更生成器 ====================

// 展开/收起配置面板
function toggleCfgPanel() {
  const toggle = document.getElementById('cfg-toggle');
  const panel = document.getElementById('cfg-panel');
  toggle.classList.toggle('open');
  panel.classList.toggle('open');
}

// 操作类型变化时，upsert 才显示从站参数和点位表
document.getElementById('cfg-op').addEventListener('change', function() {
  document.getElementById('cfg-slave-fields').style.display = (this.value === 'upsert') ? 'block' : 'none';
});

// 点位计数器
var cfgPointCounter = 0;

// 添加一个点位编辑行
function cfgAddPoint(data) {
  data = data || {};
  cfgPointCounter++;
  var div = document.createElement('div');
  div.className = 'cfg-point-item';
  div.id = 'cfgpt_' + cfgPointCounter;
  div.innerHTML =
    '<div class="cfg-point-header">' +
      '<span>点位 #' + cfgPointCounter + '</span>' +
      '<button class="cfg-point-del" onclick="this.closest(\'.cfg-point-item\').remove()">删除</button>' +
    '</div>' +
    '<div class="cfg-row">' +
      '<div class="cfg-field"><label>下标</label><input type="number" class="cfgpt-idx" value="' + (data.reg_index||0) + '" min="0" max="31"></div>' +
      '<div class="cfg-field"><label>类型</label><select class="cfgpt-type">' +
        '<option value="uint16"' + (data.type==='uint16'?' selected':'') + '>uint16</option>' +
        '<option value="int16"' + (data.type==='int16'?' selected':'') + '>int16</option>' +
        '<option value="uint32"' + (data.type==='uint32'?' selected':'') + '>uint32</option>' +
        '<option value="int32"' + (data.type==='int32'?' selected':'') + '>int32</option>' +
        '<option value="float32"' + (data.type==='float32'?' selected':'') + '>float32</option>' +
        '<option value="bit"' + (data.type==='bit'?' selected':'') + '>bit</option>' +
      '</select></div>' +
      '<div class="cfg-field"><label>系数</label><input type="number" step="any" class="cfgpt-scale" value="' + (data.scale!==undefined?data.scale:1) + '"></div>' +
      '<div class="cfg-field"><label>偏移</label><input type="number" step="any" class="cfgpt-offset" value="' + (data.offset||0) + '"></div>' +
    '</div>' +
    '<div class="cfg-row">' +
      '<div class="cfg-field"><label>名称(≤7字符)</label><input type="text" class="cfgpt-name" value="' + (data.name||'') + '" maxlength="7"></div>' +
      '<div class="cfg-field"><label>单位(≤7字符)</label><input type="text" class="cfgpt-unit" value="' + (data.unit||'') + '" maxlength="7"></div>' +
    '</div>';
  document.getElementById('cfg-points-list').appendChild(div);
}

// 生成配置变更 JSON 并填入命令输入框
function cfgGenerate() {
  var op = document.getElementById('cfg-op').value;
  var slave = { id: parseInt(document.getElementById('cfg-id').value) || 1 };

  if (op === 'upsert') {
    slave.fc = parseInt(document.getElementById('cfg-fc').value) || 3;
    slave.reg = parseInt(document.getElementById('cfg-reg').value) || 0;
    slave.count = parseInt(document.getElementById('cfg-count').value) || 1;
    slave.interval = parseInt(document.getElementById('cfg-interval').value) || 2000;
    slave.retry = parseInt(document.getElementById('cfg-retry').value) || 0;
    slave.enable = document.getElementById('cfg-enable').checked;

    // 收集点位
    var points = [];
    document.querySelectorAll('.cfg-point-item').forEach(function(pe) {
      points.push({
        reg_index: parseInt(pe.querySelector('.cfgpt-idx').value) || 0,
        type: pe.querySelector('.cfgpt-type').value,
        scale: parseFloat(pe.querySelector('.cfgpt-scale').value) || 1,
        offset: parseFloat(pe.querySelector('.cfgpt-offset').value) || 0,
        name: pe.querySelector('.cfgpt-name').value || '',
        unit: pe.querySelector('.cfgpt-unit').value || '',
        bit_index: 0
      });
    });
    if (points.length > 0) slave.points = points;
  }

  var cmd = { op: op, slaves: [slave] };
  var json = JSON.stringify(cmd);
  document.getElementById('cmd-input').value = json;

  // 自动展开命令面板提示
  showResult('ok', '✓ 已生成配置命令，填入命令框。检查后点「发送」');
}

// 加载模板
function cfgLoadTemplate(name) {
  document.getElementById('cfg-op').value = 'upsert';
  document.getElementById('cfg-slave-fields').style.display = 'block';

  var templates = {
    light: {
      id:1, fc:3, reg:2, count:2, interval:2000, retry:1, enable:true,
      points: [{reg_index:0, type:'uint32', scale:0.001, name:'light', unit:'Lux'}]
    },
    water: {
      id:2, fc:3, reg:9088, count:1, interval:1000, retry:2, enable:true,
      points: [{reg_index:0, type:'uint16', scale:1, name:'water', unit:''}]
    },
    temp: {
      id:5, fc:3, reg:0, count:2, interval:3000, retry:1, enable:true,
      points: [
        {reg_index:0, type:'int16', scale:0.1, name:'temp', unit:'C'},
        {reg_index:1, type:'uint16', scale:0.1, name:'hum', unit:'pct'}
      ]
    }
  };

  var t = templates[name];
  if (!t) return;

  document.getElementById('cfg-id').value = t.id;
  document.getElementById('cfg-fc').value = t.fc;
  document.getElementById('cfg-reg').value = t.reg;
  document.getElementById('cfg-count').value = t.count;
  document.getElementById('cfg-interval').value = t.interval;
  document.getElementById('cfg-retry').value = t.retry;
  document.getElementById('cfg-enable').checked = t.enable;

  // 清空旧点位，加载新点位
  document.getElementById('cfg-points-list').innerHTML = '';
  cfgPointCounter = 0;
  t.points.forEach(function(pt) { cfgAddPoint(pt); });

  cfgGenerate();
}

// 初始化：添加一个默认点位
cfgAddPoint({reg_index:0, type:'uint32', scale:0.001, name:'light', unit:'Lux'});

// 下发命令
let pendingCmdTime = null;  // 记录上次发命令的时间，用于匹配 ack
async function sendCmd() {
  const input = document.getElementById('cmd-input');
  const result = document.getElementById('cmd-result');
  const btn = document.getElementById('send-btn');
  const msg = input.value.trim();

  if (!msg) { showResult('err', '请输入命令'); return; }

  btn.disabled = true;
  result.className = 'cmd-result';
  result.textContent = '发送中...';
  result.style.display = 'block';

  try {
    const resp = await fetch('/api/publish', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({msg: msg})
    });
    const data = await resp.json();
    if (data.ok) {
      pendingCmdTime = Date.now();
      showResult('ok', '✓ 已发送到百度云  ' + data.detail + '  ⏳ 等待板子回执...');
    } else {
      showResult('err', '✗ 发送失败  ' + (data.detail || data.error || ''));
    }
  } catch(e) {
    showResult('err', '✗ 请求失败  ' + e.message);
  }
  btn.disabled = false;
}

function showResult(type, text) {
  const el = document.getElementById('cmd-result');
  el.className = 'cmd-result ' + type;
  el.textContent = text;
}

// 收到 cmd_ack 时更新命令面板
function handleCmdAck(msg) {
  const elapsed = pendingCmdTime ? ((Date.now() - pendingCmdTime) / 1000).toFixed(1) + 's' : '';
  const parsed = msg.parsed;
  if (!parsed) {
    showResult('ok', '板子回执: ' + msg.payload);
    pendingCmdTime = null;
    return;
  }
  // 根据 type 字段判断命令类型
  const typeLabel = parsed.type === 'config' ? '配置变更'
                  : (parsed.type === 'ota' ? 'OTA升级' : '写寄存器');
  // 错误码中文翻译
  const errMap = {
    '-1': 'JSON解析失败',
    '-2': '升级静默期拒绝',
    '-3': '部分从站应用失败',
    'bad_cmd': '命令参数错误'
  };
  if (parsed.status === 'ok') {
    showResult('ok', '✓ ' + typeLabel + '成功 ' + elapsed + '  ' + msg.payload);
  } else if (parsed.status === 'accepted') {
    showResult('ok', '✓ ' + typeLabel + '已受理 ' + elapsed + '  ' + msg.payload);
  } else if (parsed.status === 'busy') {
    showResult('err', '✗ ' + typeLabel + '被拒绝(设备忙/升级静默期) ' + elapsed + '  ' + msg.payload);
  } else if (parsed.status === 'err') {
    const errDesc = errMap[String(parsed.err)] || parsed.err || '?';
    showResult('err', '✗ ' + typeLabel + '失败 ' + elapsed + '  错误: ' + errDesc + '  ' + msg.payload);
  } else {
    showResult('ok', '板子回执: ' + msg.payload);
  }
  pendingCmdTime = null;
}

// 回车发送
document.getElementById('cmd-input').addEventListener('keydown', e => {
  if (e.key === 'Enter') sendCmd();
});

// === 实时数据更新 ===
let slaveData = {};  // {slave_id: {data, ts}}
let msgCount = 0;

function updateSlaveList() {
  const container = document.getElementById('slave-list');
  const ids = Object.keys(slaveData).sort((a,b) => Number(a) - Number(b));

  if (ids.length === 0) {
    container.innerHTML = '<div class="no-data">等待数据...</div>';
    return;
  }

  container.innerHTML = '';
  for (const sid of ids) {
    const info = slaveData[sid];
    const d = info.data;
    const online = d.online === 1 || d.online === true;
    const pts = d.pts || [];

    const card = document.createElement('div');
    card.className = 'slave-card flash';
    setTimeout(() => card.classList.remove('flash'), 500);

    let ptsHtml = '';
    for (const p of pts) {
      ptsHtml += `<div class="point-row">
        <span class="point-name">${p.n || '?'}</span>
        <span><span class="point-value">${p.v}</span><span class="point-unit">${p.u || ''}</span></span>
      </div>`;
    }

    card.innerHTML = `
      <div class="slave-card-header">
        <span class="slave-id">Slave ${sid}</span>
        <span class="${online ? 'slave-online' : 'slave-offline'}">${online ? '● 在线' : '● 离线'}</span>
        <span class="slave-ts">${info.ts}</span>
      </div>
      ${ptsHtml || '<div class="no-data">无点位</div>'}
    `;
    container.appendChild(card);
  }
  document.getElementById('slave-count').textContent = ids.length;
}

function addMessage(msg) {
  msgCount++;
  document.getElementById('msg-count').textContent = msgCount;

  // cmd_ack 消息：更新命令面板的回执显示
  if (msg.msg_type === 'cmd_ack') {
    handleCmdAck(msg);
  }

  const list = document.getElementById('msg-list');
  // 清除"等待数据"
  if (list.querySelector('.no-data')) list.innerHTML = '';

  const item = document.createElement('div');
  item.className = 'msg-item' + (msg.msg_type === 'cmd_ack' ? ' msg-ack' : '');

  let bodyText = msg.payload;
  if (msg.parsed) {
    bodyText = JSON.stringify(msg.parsed, null, 2);
  }

  // cmd_ack 消息加标签
  const typeTag = msg.msg_type === 'cmd_ack' ? '<span class="msg-tag ack">ACK</span>' : '';

  item.innerHTML = `
    <div class="msg-meta">
      <span class="msg-ts">${msg.ts}</span>
      ${typeTag}
      <span class="msg-topic">${msg.topic}</span>
    </div>
    <div class="msg-body">${escapeHtml(bodyText)}</div>
  `;
  list.insertBefore(item, list.firstChild);

  // 更新 slave 数据（只处理遥测消息）
  if (msg.msg_type === 'telemetry' && msg.parsed && msg.parsed.slave !== undefined) {
    slaveData[String(msg.parsed.slave)] = {
      data: msg.parsed,
      ts: msg.ts
    };
    updateSlaveList();
  }

  // 限制消息数量
  while (list.children.length > 200) {
    list.removeChild(list.lastChild);
  }
}

function escapeHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

// === SSE 连接 ===
function connectSSE() {
  const es = new EventSource('/api/stream');

  es.onmessage = function(e) {
    try {
      const msg = JSON.parse(e.data);
      addMessage(msg);
    } catch(err) {}
  };

  es.onerror = function() {
    es.close();
    setTimeout(connectSSE, 2000);  // 断线重连
  };
}

// === 轮询状态 ===
async function pollStatus() {
  try {
    const resp = await fetch('/api/status');
    const data = await resp.json();

    const dot = document.getElementById('mqtt-dot');
    const statusText = document.getElementById('mqtt-status');
    if (data.mqtt.connected) {
      dot.className = 'status-dot online';
      statusText.textContent = 'MQTT 已连接 ' + (data.mqtt.connect_time || '');
    } else {
      dot.className = 'status-dot offline';
      statusText.textContent = 'MQTT ' + (data.mqtt.error || '未连接');
    }

    document.getElementById('msg-count').textContent = data.msg_count;

    // 用 status 里的 slave 数据更新（SSE 未连上时的兜底）
    if (data.slaves && Object.keys(data.slaves).length > 0 && Object.keys(slaveData).length === 0) {
      for (const sid in data.slaves) {
        slaveData[sid] = {data: data.slaves[sid].data, ts: data.slaves[sid].ts};
      }
      updateSlaveList();
    }
  } catch(e) {}
}

// 启动
connectSSE();
pollStatus();
setInterval(pollStatus, 3000);
</script>
</body>
</html>
"""


# ==================== 启动 ====================

def main():
    import argparse
    parser = argparse.ArgumentParser(description="STM32 MQTT Gateway Web 控制台")
    parser.add_argument("--port", type=int, default=WEB_PORT, help="Web 服务端口")
    parser.add_argument("--host", default="127.0.0.1", help="监听地址（默认 127.0.0.1）")
    args = parser.parse_args()

    print("=" * 60)
    print("  STM32 MQTT Gateway Web 控制台")
    print("=" * 60)
    print("  百度云 Broker : {}:{}".format(HOST, MQTT_PORT))
    print("  订阅身份      : {} (topics: {})".format(SUB_DEVICE_KEY, ", ".join(SUB_TOPICS)))
    print("  下发目标      : {} (topic: {})".format(PUB_DEVICE_KEY, PUB_TOPIC))
    print("  Web 地址      : http://{}:{}".format(args.host, args.port))
    print("=" * 60)

    # 启动 MQTT 订阅线程
    t = threading.Thread(target=mqtt_subscriber_thread, daemon=True)
    t.start()

    # 启动 HTTP Server
    httpd = ThreadingHTTPServer((args.host, args.port), UIHandler)
    print("\n[WEB] 服务已启动，浏览器打开 http://{}:{}".format(args.host, args.port))
    print("[WEB] Ctrl+C 退出\n")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[WEB] 退出")
        httpd.shutdown()


if __name__ == "__main__":
    main()
