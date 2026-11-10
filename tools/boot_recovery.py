"""Boot本地串口恢复工具；设备必须配置与安全包匹配的可信密钥。"""

import argparse
from pathlib import Path
import struct
import time


# 发送 8 B 命令及小端参数，确认请求已完整交给串口。
def send_request(port, command, argument=0):
    request = command + struct.pack("<I", argument)
    if len(command) != 8 or port.write(request) != len(request):
        raise IOError("恢复命令未完整发送")
    port.flush()


# 在固定期限内等待指定响应前缀，严格模式拒绝非预期恢复状态。
def wait_line(port, prefix, seconds, strict=True):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = port.readline().decode("ascii", errors="replace").strip()
        if not line:
            continue
        print(line)
        if line.startswith(prefix):
            return line
        if strict and line.startswith("RCV ") and not line.startswith(("RCV SLOT ", "RCV ERASING ", "RCV VERIFYING")):
            raise RuntimeError(f"恢复失败或响应阶段不符：{line}")
    raise TimeoutError(f"等待 {prefix} 超时；Boot 会保留恢复状态，不要把超时当作成功")


# 查询设备包槽、升级日志及持久兼容版本，拒绝不完整或不支持的状态。
def query_status(port):
    # 尚未发送请求，先等待新的接收窗口；不把上次事务残留响应当作本次结果。
    wait_line(port, "PKG WAIT", 900, strict=False)
    send_request(port, b"GWRCINFO")
    line = wait_line(port, "RCV INFO ", 10)
    try:
        fields = dict(item.split("=", 1) for item in line.split()[2:])
        status = {key: int(fields[key]) for key in
                  ("normal", "recovery", "phase", "sequence", "protected", "source")}
    except (KeyError, ValueError) as error:
        raise RuntimeError("设备状态响应不完整") from error
    if (status["normal"] not in (0, 1, 2, 3, 5) or status["recovery"] not in (0, 1, 2, 3, 5)
            or status["phase"] not in range(8) or status["protected"] not in range(3)
            or status["source"] not in range(3) or not 0 <= status["sequence"] <= 0xFFFFFFFF):
        raise RuntimeError("设备状态值不受当前协议支持")
    if "compat" in fields:
        status["compat"] = int(fields["compat"])
        if status["compat"] not in (1, 2):
            raise RuntimeError("设备持久基线不受支持")
    for key in ("tx_state", "tx_generation", "normal_phase", "normal_sequence", "attempts"):
        if key in fields:
            status[key] = int(fields[key])
    if "tx_state" in status and status["tx_state"] not in (0, 1, 2, 3, 5):
        raise RuntimeError("事务号日志状态不受支持")
    return status


def check_package(package, persistence_version=1):
    """发送前只做格式准入检查，不能替代设备端的完整认证。"""
    if persistence_version not in (1, 2):
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
def read_package(path, persistence_version=1):
    package = path.read_bytes()
    check_package(package, persistence_version)
    return package


# 核对设备保护状态，向指定槽发送恢复包并等待首次试运行结果。
def recover(port, package, slot, allow_unknown=False, persistence_version=1):
    if slot not in ("A", "B"):
        raise ValueError("恢复槽只能为 A 或 B")
    check_package(package, persistence_version)
    status = query_status(port)
    if status.get("compat", 1) != persistence_version:
        raise RuntimeError("设备、包与选择的持久基线不一致，未发送擦写请求")
    if status["normal"] == 5 or status["recovery"] == 5:
        raise RuntimeError("现有日志版本不支持，普通恢复不能隐式迁移或清除它")
    selected = 1 if slot == "A" else 2
    if status["normal"] == 2 or status["recovery"] == 2:
        raise RuntimeError("元数据读取失败，禁止擦写；--allow-unknown-state 也不能绕过")
    if status["protected"] == selected:
        raise RuntimeError(f"槽 {slot} 是可信已确认包，禁止覆盖；请选择另一槽")
    if (status["normal"] == 3 or status["recovery"] == 3) and not allow_unknown:
        raise RuntimeError("元数据损坏；核对需保留内容后，显式指定 --allow-unknown-state")
    start = 0x180000 if selected == 1 else 0x280000
    print(f"恢复将擦写槽 {slot}：[0x{start:06X}, 0x{start + 0x100000:06X})，容量 1 MiB。")
    print("另使用恢复日志区 [0x382000, 0x385000)；健康确认成功后才重建普通升级日志。")
    command = (b"GWRCFR" if allow_unknown else b"GWRCVR") + slot.encode("ascii") + b"1"
    send_request(port, command, len(package))
    # 256个4KiB扇区，每次有界等待及读回；这是等待上限，不是固定延时。
    ready = wait_line(port, "RCV READY ", 360)
    if ready != f"RCV READY {selected} {len(package)}":
        raise RuntimeError("设备目标槽或长度与请求不符")
    for offset in range(0, len(package), 256):
        chunk = package[offset:offset + 256]
        if port.write(chunk) != len(chunk):
            raise IOError(f"数据未完整发送，offset={offset}")
        port.flush()
        if wait_line(port, "RCV ACK ", 5) != f"RCV ACK {offset + len(chunk)}":
            raise RuntimeError("设备写入进度不符")
    verified = wait_line(port, "RCV VERIFIED ", 210)
    if verified != f"RCV VERIFIED {len(package) - 208} {selected}":
        raise RuntimeError("设备认证结果与请求不符")
    # 覆盖源包认证180s、擦除/安装/摘要240s及持久提交；不是实测耗时。
    result = wait_line(port, "RCV RESULT ", 480)
    if result != "RCV RESULT 1":
        raise RuntimeError(f"候选已保存，但安装未进入首次试运行：{result}")
    print("安装和片内校验完成，正在首次试运行；尚未完成产品健康确认。")
    print("无可信旧包时，未确认重启将留在 Boot 等待恢复，不会自动信任本包。")


# 解析恢复操作参数，独占串口完成查询、首次配置或恢复。
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="实际串口，例如 COM11")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--info", action="store_true", help="只查询，设备留在恢复服务等待命令")
    action.add_argument("--run", action="store_true", help="重试持久事务；正常状态下退出服务启动 App")
    action.add_argument("--initialize-transactions", action="store_true",
                        help="基线2首次配置事务号；要求全部升级元数据为空白，不擦除已有数据")
    action.add_argument("--package", type=Path, help="要恢复的加密 .upd 包")
    parser.add_argument("--erase-slot", choices=("A", "B"), help="明确允许覆盖的整个固件包槽")
    parser.add_argument("--allow-unknown-state", action="store_true", help="显式允许处理损坏元数据，不绕过读错误或旧包保护")
    parser.add_argument("--persistence-version", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    if args.package and not args.erase_slot:
        parser.error("发送恢复包必须明确指定 --erase-slot A 或 B")
    if not args.package and (args.erase_slot or args.allow_unknown_state):
        parser.error("擦写选项只能与 --package 一起使用")
    if args.initialize_transactions and args.persistence_version != 2:
        parser.error("事务号首次配置仅适用于明确选择的持久基线2")
    package = read_package(args.package, args.persistence_version) if args.package else None
    import serial

    print("关闭串口监视器；本工具独占串口。打开后复位设备，Boot须已配置信任映射。")
    with serial.Serial(args.port, 115200, timeout=0.5, write_timeout=3) as port:
        if package is not None:
            recover(port, package, args.erase_slot, args.allow_unknown_state, args.persistence_version)
        else:
            status = query_status(port)
            print(status)
            if args.initialize_transactions:
                if status.get("compat", 1) != 2:
                    raise RuntimeError("设备不是持久基线2，未发送初始化请求")
                if (status["normal"] != 0 or status["recovery"] != 0 or status.get("tx_state") != 0):
                    raise RuntimeError("升级元数据不是首次全空状态，未发送初始化请求")
                send_request(port, b"GWRTINIT")
                if wait_line(port, "RCV TX_INIT ", 15) != "RCV TX_INIT result=1":
                    raise RuntimeError("事务号未完成首次配置；不清空或重试擦除")
            if args.run:
                send_request(port, b"GWRCRUN1")
                result = wait_line(port, "RCV RESULT ", 900)
                if result not in ("RCV RESULT 0", "RCV RESULT 1", "RCV RESULT 2"):
                    raise RuntimeError(f"设备仍留在恢复服务：{result}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        raise SystemExit(f"FAILED: {error}") from error
