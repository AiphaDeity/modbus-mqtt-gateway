#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
百度云 IoT Core HTTP 下发工具（不抢 MQTT 连接，板子在线时安全使用）

用法:
    python tools/baidu_pub.py '{"slave":2,"func":6,"addr":0,"val":100}'
    python tools/baidu_pub.py '{"slave":1,"func":5,"addr":0,"val":1}' --topic $iot/stm32_gw_01/user/commands
    python tools/baidu_pub.py --topic $iot/stm32_gw_01/user/acks "任意消息"

默认 topic = $iot/stm32_gw_01/user/commands（M2 下行命令 topic）
默认 qos  = 0

原理:
    1. 用设备三元组算 username/password（算法和 MQTT 连接一致）
    2. POST /auth 换 token
    3. POST /pub?topic=...&qos=0 发消息（服务端注入，不开 MQTT 长连接，不会踢板子）

⚠️ 本脚本里的三元组是真实凭据，git commit 前必须改回占位符！
"""
import sys
import json
import time
import hashlib
import urllib.request
import urllib.parse
import urllib.error

# ===== 设备三元组（从 firmware/app/integration/mqtt_client.c 抄过来，保持一致）=====
# ⚠️ git commit 前改成 "xxxxxxx" 占位符
IOTCORE_ID    = "aeklwww"
DEVICE_KEY    = "stm32_gw_01"
DEVICE_SECRET = "YOUR_DEVICE_SECRET"    # git commit 前替换为占位符
REGION        = "gz"                       # 广州地域，和实例一致

HOST = "{}.iot.{}.baidubce.com".format(IOTCORE_ID, REGION)
DEFAULT_TOPIC = "$iot/{}/user/commands".format(DEVICE_KEY)


def build_username_password(ts):
    """百度云 IoT Core 认证参数（和 MQTT CONNECT 的 username/password 算法一致）"""
    username = "thingidp@{}|{}|{}|SHA256".format(IOTCORE_ID, DEVICE_KEY, ts)
    sign_content = "{}&{}&SHA256{}".format(DEVICE_KEY, ts, DEVICE_SECRET)
    password = hashlib.sha256(sign_content.encode("utf-8")).hexdigest()
    return username, password


def get_token(username, password, lifespan=300):
    """POST /auth 换 token（HTTP 80，官方文档支持的非加密接入）"""
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
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        print("[AUTH] HTTP {} : {}".format(e.code, body))
        return None
    except Exception as e:
        print("[AUTH] error: {}".format(e))
        return None


def publish(topic, msg, qos=0):
    """POST /pub 发消息"""
    ts = int(time.time())
    username, password = build_username_password(ts)
    print("[AUTH] ts={} user={}...".format(ts, username[:30]))

    token = get_token(username, password)
    if not token:
        print("[AUTH] 获取 token 失败，检查三元组/网络")
        return False
    print("[AUTH] token OK (len={})".format(len(token)))

    url = "http://{}/pub?topic={}&qos={}".format(
        HOST, urllib.parse.quote(topic, safe=""), qos)
    req = urllib.request.Request(
        url, data=msg.encode("utf-8"),
        headers={
            "token": token,
            "Content-Type": "application/octet-stream",
            "Accept": "application/json",
        }
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            resp = r.read().decode("utf-8", errors="replace")
            print("[PUB]  HTTP {} : {}".format(r.status, resp))
            return True
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        print("[PUB]  HTTP {} : {}".format(e.code, body))
        return False
    except Exception as e:
        print("[PUB]  error: {}".format(e))
        return False


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        print("当前配置:")
        print("  IoTCoreId : {}".format(IOTCORE_ID))
        print("  DeviceKey : {}".format(DEVICE_KEY))
        print("  Region    : {}  (host={})".format(REGION, HOST))
        print("  默认topic : {}".format(DEFAULT_TOPIC))
        return

    # 解析 --topic
    topic = DEFAULT_TOPIC
    if "--topic" in args:
        i = args.index("--topic")
        topic = args[i + 1]
        args = args[:i] + args[i + 2:]

    msg = " ".join(args) if args else ""
    if not msg:
        print("错误：未提供消息内容")
        sys.exit(1)

    print("目标: {}".format(HOST))
    print("topic: {}".format(topic))
    print("消息: {}".format(msg))
    print("-" * 50)
    ok = publish(topic, msg)
    print("-" * 50)
    if ok:
        print("结果: 发送成功 ✓")
    else:
        print("结果: 发送失败 ✗")
        sys.exit(1)


if __name__ == "__main__":
    main()
