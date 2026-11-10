#!/usr/bin/env bash
# 一键烧录 STM32 固件到板子（ST-Link SWD）
# 用法: bash flash.sh [elf路径]
# 默认烧录 build/Debug/firmware/app/modbus-mqtt-gateway.elf
set -e

CP="D:/stm32cube/STM32CubeIDE_1.17.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.0.202409170845/tools/bin/STM32_Programmer_CLI.exe"

ELF="${1:-build/Debug/firmware/app/modbus-mqtt-gateway.elf}"

if [ ! -f "$ELF" ]; then
  echo "ERROR: elf 文件不存在: $ELF"
  exit 1
fi

echo "=== 烧录: $ELF ==="
"$CP" -c port=SWD reset=HP -e all -w "$ELF" -go
echo "=== 烧录完成 ==="
