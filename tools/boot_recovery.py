"""Boot维护帧V1恢复工具；设备必须配置与安全包匹配的可信密钥。"""

import argparse
import json
from pathlib import Path
import secrets
import struct
import time
import zlib


# 帧头与固件maint_frame一致；CRC覆盖version到payload。
def encode_frame(opcode, request_id, payload):
    if len(payload) > 1024:
        raise ValueError("维护帧载荷超过1024 B")
    body = struct.pack("<BBBIH", 1, opcode, 0, request_id, len(payload)) + payload
    return b"\xa5\x5a" + body + struct.pack("<I", zlib.crc32(body))


class RecoveryLink:
    """单次串口连接的帧缓存和请求关联，不自动重试擦写操作。"""

    def __init__(self, port):
        self.port = port
        self.request_id = secrets.randbelow(0xFFFFFFFE) + 1
        self.buffer = bytearray()
        self.partial_started = None

    # 每条请求使用新编号；分块载荷由调用方提供，其余为JSON。
    def send(self, opcode, arguments):
        self.request_id = (self.request_id + 1) & 0xFFFFFFFF
        payload = arguments if isinstance(arguments, bytes) else json.dumps(
            arguments, separators=(",", ":"), ensure_ascii=True).encode("ascii")
        frame = encode_frame(opcode, self.request_id, payload)
        if self.port.write(frame) != len(frame):
            raise IOError("维护帧未完整发送")
        self.port.flush()
        return self.request_id

    # 有界等待、过滤日志/迟到应答，并校验CRC及帧内外的请求号。
    def wait(self, opcode, request_id, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.partial_started is not None and time.monotonic() - self.partial_started >= 2:
                self.buffer.clear()
                self.partial_started = None
            if self.buffer:
                index = self.buffer.find(b"\xa5\x5a")
                if index < 0:
                    self.buffer[:] = b"\xa5" if self.buffer[-1] == 0xA5 else b""
                elif index:
                    del self.buffer[:index]
                if len(self.buffer) >= 11:
                    version, received_op, flags, received_id, length = struct.unpack_from(
                        "<BBBIH", self.buffer, 2)
                    if version != 1 or flags != 1 or length > 1024:
                        del self.buffer[0]
                        continue
                    total = 15 + length
                    if len(self.buffer) >= total:
                        wire = bytes(self.buffer[:total])
                        if zlib.crc32(wire[2:-4]) != struct.unpack_from("<I", wire, total - 4)[0]:
                            del self.buffer[0]
                            continue
                        del self.buffer[:total]
                        self.partial_started = None
                        if received_op != opcode or received_id != request_id:
                            continue
                        try:
                            response = json.loads(wire[11:-4].decode("utf-8"))
                        except (ValueError, UnicodeError) as error:
                            raise RuntimeError("设备响应不是合法JSON") from error
                        if (not isinstance(response, dict)
                                or type(response.get("request_id")) is not int
                                or response["request_id"] != request_id
                                or response.get("status") not in ("OK", "ACCEPTED", "ERROR")
                                or "error" not in response):
                            raise RuntimeError("设备响应字段不完整或请求号不匹配")
                        if response["status"] == "ERROR" or response["error"] is not None:
                            raise RuntimeError(f"设备拒绝：{response}")
                        return response
            before = time.monotonic()
            chunk = self.port.read(1)
            if chunk:
                if not self.buffer:
                    self.partial_started = before
                self.buffer.extend(chunk)
        raise TimeoutError("等待维护帧响应超时；未确认成功，不自动重发擦写请求")

    # 请求与响应串行，只有完成本次调用才能复用缓冲或发送下一块。
    def request(self, opcode, arguments, seconds=10):
        request_id = self.send(opcode, arguments)
        return self.wait(opcode, request_id, seconds)


# 仅只读INFO允许重试等待设备复位；收到响应后严格核对模式与持久版本。
def query_status(link):
    deadline = time.monotonic() + 900
    while True:
        try:
            status = link.request(0x01, {}, min(2, max(0, deadline - time.monotonic())))
            break
        except TimeoutError:
            if time.monotonic() >= deadline:
                raise
    if status.get("mode") != "BOOT" or status.get("compat") != 2:
        raise RuntimeError("设备不是Boot恢复模式或持久版本不是2，未发送擦写请求")
    for key, allowed in (("normal", (0, 1, 2, 3, 5)), ("recovery", (0, 1, 2, 3, 5)),
                         ("phase", range(8)), ("protected", range(3)),
                         ("source", range(3)), ("tx_state", (0, 1, 2, 3, 5))):
        if type(status.get(key)) is not int or status[key] not in allowed:
            raise RuntimeError(f"设备状态字段无效：{key}")
    return status


def check_package(package, persistence_version=2):
    """发送前只做格式准入检查，不能替代设备端的完整认证。"""
    if persistence_version != 2:
        raise ValueError("不支持的持久兼容版本")
    if not 216 <= len(package) <= 448 * 1024 + 208 or package[:8] != b"GWUPD001":
        raise ValueError("输入必须是包含 App 向量的 V2 加密 .upd 包，不能是原始 bin")
    if (struct.unpack_from("<HH", package, 8) != (2, 128)
            or struct.unpack_from("<H", package, 114)[0] != 1
            or any(package[116:128])):
        raise ValueError("当前 Boot 只接受 V2/IWDG_HANDOFF，且保留区必须为零")
    if (package[12:28] != b"F407-TEST".ljust(16, b"\x00")
            or struct.unpack_from("<I", package, 32)[0] != 0x08010000
            or struct.unpack_from("<I", package, 44)[0] != 1
            or struct.unpack_from("<IH", package, 108) != (1, persistence_version)):
        raise ValueError("硬件标识、App 地址或密钥/兼容编号与所选 Boot 基线不符")
    plain_size, cipher_size = struct.unpack_from("<II", package, 36)
    if plain_size != cipher_size or cipher_size + 208 != len(package):
        raise ValueError("包头与实际长度不符")


# 读取加密包并执行发送前格式检查。
def read_package(path, persistence_version=2):
    package = path.read_bytes()
    check_package(package, persistence_version)
    return package


# 核对保护状态，经BEGIN/CHUNK/FINISH发送同一安全包；设备仍执行完整认证。
def recover(link, package, slot, allow_unknown=False, persistence_version=2):
    if slot not in ("A", "B"):
        raise ValueError("恢复槽只能为 A 或 B")
    check_package(package, persistence_version)
    status = query_status(link)
    if status["normal"] == 5 or status["recovery"] == 5:
        raise RuntimeError("现有日志版本不支持，普通恢复不能隐式迁移或清除它")
    if status["normal"] == 2 or status["recovery"] == 2:
        raise RuntimeError("元数据读取失败，禁止擦写")
    selected = 1 if slot == "A" else 2
    if status["protected"] == selected:
        raise RuntimeError(f"槽 {slot} 是可信已确认包，禁止覆盖")
    if (status["normal"] == 3 or status["recovery"] == 3) and not allow_unknown:
        raise RuntimeError("元数据损坏，需显式指定 --allow-unknown-state")
    start = 0x180000 if selected == 1 else 0x280000
    print(f"恢复擦写槽 {slot}：[0x{start:06X}, 0x{start + 0x100000:06X})。")
    print("恢复元数据使用 [0x382000, 0x385000)，健康确认后才重建普通日志。")
    ready = link.request(0x30, {"slot": selected, "total_len": len(package),
                               "allow_unknown_state": bool(allow_unknown)}, 360)
    session_id = ready.get("session_id")
    if (ready["status"] != "OK" or type(session_id) is not int or not 0 < session_id <= 0xFFFFFFFF
            or ready.get("slot") != selected or ready.get("total_len") != len(package)
            or ready.get("next_offset") != 0):
        raise RuntimeError("设备目标槽、会话或长度与请求不符")
    for offset in range(0, len(package), 256):
        chunk = package[offset:offset + 256]
        response = link.request(0x31, struct.pack("<II", session_id, offset) + chunk, 5)
        if (response["status"] != "OK" or response.get("session_id") != session_id
                or response.get("next_offset") != offset + len(chunk)):
            raise RuntimeError("设备会话或写入进度不符")
    request_id = link.send(0x32, {"session_id": session_id})
    accepted = link.wait(0x32, request_id, 210)
    if accepted["status"] != "ACCEPTED" or accepted.get("phase") != "READY":
        raise RuntimeError("设备没有确认安全包已持久提交")
    result = link.wait(0x32, request_id, 480)
    if result["status"] != "OK" or result.get("install_result") != 1 or result.get("phase") != "TRIAL":
        raise RuntimeError("安装未进入首次试运行")
    print("安装和片内校验完成，准备首次试运行；尚未完成实际启动和产品健康确认。")


# 工具独占串口；查询不擦写，初始化和恢复必须由命令行明确选择。
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="实际串口，例如 COM11")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--info", action="store_true", help="只查询，设备留在恢复服务")
    action.add_argument("--run", action="store_true", help="重启Boot，重新判断持久事务及App")
    action.add_argument("--initialize-transactions", action="store_true",
                        help="首次配置事务号；要求全部升级元数据为空白，不擦除已有数据")
    action.add_argument("--package", type=Path, help="加密 .upd 包")
    parser.add_argument("--erase-slot", choices=("A", "B"), help="明确允许覆盖的整个包槽")
    parser.add_argument("--allow-unknown-state", action="store_true", help="允许损坏元数据，不绕过读错误或旧包保护")
    parser.add_argument("--persistence-version", type=int, choices=(2,), default=2)
    args = parser.parse_args()
    if args.package and not args.erase_slot:
        parser.error("发送恢复包必须明确指定 --erase-slot A 或 B")
    if not args.package and (args.erase_slot or args.allow_unknown_state):
        parser.error("擦写选项只能与 --package 一起使用")
    package = read_package(args.package, args.persistence_version) if args.package else None
    import serial

    print("连接USART2维护口：USB转串口RX接PD5、TX接PD6，共地，使用3.3V电平。")
    print("关闭维护口串口监视器；工具独占该口，打开后复位设备。USART1只输出日志。")
    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=3) as port:
        link = RecoveryLink(port)
        if package is not None:
            recover(link, package, args.erase_slot, args.allow_unknown_state, args.persistence_version)
        else:
            status = query_status(link)
            print(status)
            if args.initialize_transactions:
                if status["normal"] != 0 or status["recovery"] != 0 or status["tx_state"] != 0:
                    raise RuntimeError("升级元数据不是首次全空状态，未发送初始化请求")
                result = link.request(0x30, {"initialize_transactions": True}, 15)
                if result["status"] != "OK":
                    raise RuntimeError("事务号未完成首次配置")
            if args.run:
                result = link.request(0x16, {})
                if result["status"] != "OK":
                    raise RuntimeError("设备未确认重启请求")
                print("设备已接收重启请求；App是否启动须核对后续运行结果。")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        raise SystemExit(f"FAILED: {error}") from error
