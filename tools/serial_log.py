#!/usr/bin/env python3
"""串口日志读取 —— 后台持续读 STM32 USART 输出，带时间戳。
用法: python serial_log.py <COM口> [波特率] [输出文件]
示例: python serial_log.py COM3 115200 serial.log
按 Ctrl+C 停止。
"""
import sys, time, datetime
try:
    import serial
except ImportError:
    print("ERROR: pyserial 未安装，运行: pip install pyserial")
    sys.exit(1)

def main():
    if len(sys.argv) < 2:
        print("用法: python serial_log.py <COM口> [波特率] [输出文件]")
        print("示例: python serial_log.py COM3 115200 serial.log")
        sys.exit(1)
    port = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    outfile = sys.argv[3] if len(sys.argv) > 3 else None

    print(f"[serial_log] 打开 {port} @ {baud}, 输出={'stdout+文件:'+outfile if outfile else 'stdout'}, Ctrl+C 停止", flush=True)
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
    except Exception as e:
        print(f"[serial_log] 打开 {port} 失败: {e}", flush=True)
        sys.exit(1)

    f = open(outfile, "a", encoding="utf-8", errors="replace") if outfile else None
    start = time.time()
    try:
        while True:
            data = ser.read(256)
            if not data:
                continue
            try:
                text = data.decode("utf-8", errors="replace")
            except Exception:
                text = repr(data)
            ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            line = f"{ts} {text}"
            sys.stdout.write(line)
            sys.stdout.flush()
            if f:
                f.write(line)
                f.flush()
    except KeyboardInterrupt:
        elapsed = time.time() - start
        print(f"\n[serial_log] 停止, 运行 {elapsed:.1f}s", flush=True)
    finally:
        ser.close()
        if f:
            f.close()

if __name__ == "__main__":
    main()
